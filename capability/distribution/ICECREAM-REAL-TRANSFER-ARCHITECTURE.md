# Protocol-50 transfer architecture in real Icecream

Status: implementation-strategy draft for issue #16, prepared for bigoracle review. This
document distinguishes current Icecream, the M5 capability program, the distribution simulator,
and the smallest product architecture for the interning codec. The central decision is that
compile-job connections carry job references and results, while a persistent cache-to-cache
channel carries DICT, LINES, Need, and Fill.

Nothing waits for a whole build. A complete-TU preparation boundary remains on C for the first
implementation, but many TUs occupy different pipeline stages concurrently.

## Decisions in one page

1. Current Icecream streams preprocessor bytes directly from one C client to one F job child.
   Protocol 50 keeps that job connection for execution metadata, diagnostics, object data, and
   the final result, but replaces source-byte transfer with a cache transaction reference.
2. A persistent C cache lives beside C `iceccd`; a persistent F cache lives beside F `iceccd`.
   They share the interner and definition implementation.
3. Each cache-store incarnation receives a startup GUID. A cache relationship is identified by
   `(C_STORE_GUID, F_STORE_GUID)`. A TU is named `(C_STORE_GUID, TU_SEQ)`; `TU_SEQ` is unique
   within that C-store lifetime and records the C cache's total PreparedTU admission order.
   `REL_SEQ` is the contiguous ordinal in one destination F's order-preserving projection of
   that total order.
4. A C clone sends `CompileFileMsg` with a cache-reference input descriptor containing
   `(C_STORE_GUID, TU_SEQ)`. The F clone attaches that job to the same key in its local F cache.
   The job connection never carries DICT, LINES, or Fill payload.
5. C cache and F cache exchange `DICT + LINES / NEED / FILL` over one persistent, multiplexed,
   full-duplex channel per active relationship. DICT contains the complete dependency manifest.
   F sends Need at `DICT_END` while C continues sending LINES, and C queues Fill as soon as Need
   is processed. One dialogue is active per relationship initially; different F relationships
   advance concurrently.
6. Root, Fill, and installed definitions are immutable and idempotent. No hidden state crosses a
   relationship. Route-local history belongs to one `(C_GUID, F arena)` and advances only in
   `REL_SEQ` order. Globally reusable material is explicitly named and immutable.
7. Clone lifetime and cache lifetime are independent. A clone ending detaches one compiler
   consumer; it does not cancel cache transfer or delete Root, definitions, or learned blocks.
8. F cache retains a completed Root and its reusable objects under its normal cache policy.
   It creates reconstructed bytes only for an attached job and may discard that materialized
   byte buffer after consumption.
9. The C uplink schedules bounded frames across the active head transaction of every F
   relationship. Fill and completion traffic may move ahead of lower-priority body traffic, but
   bounded priority bursts give an older flow a turn. A relationship starts TU `q+1` when TU `q`
   commits; compilation of `q` may continue while `q+1` transfers.
10. The scored size is every C-to-F socket byte, including cache-channel framing and job-control
    references. F-to-C Need and acknowledgement bytes are timed and logged separately.
11. The existing transfer remains a complete fallback. Selection occurs before a TU begins its
    remote input transfer; the implementation never splices two input formats into one attempt.
12. Standalone Asio on C++23 is the leading cache-I/O implementation. Whether an F gives each
    active C GUID a process, a dedicated thread/context, or a shared-context strand remains open
    for review and measurement.
13. Simulator topology names distinguish a shared C uplink, each F ingress, and an optional
    per-route ceiling. `C1F20_200B1G` means one C, twenty Fs, 200 slots on every F, and
    one shared 1-Gbit/s C uplink; it does not create twenty independent 1-Gbit/s C uplinks.
14. The production implementation and simulator are parallel tracks. The simulator compares
    P29, GRZ, routing, and cache distribution; it does not replace implementation of cache
    services, protocol messages, or clone rendezvous in Icecream.

## Review status and unresolved choices

The following are proposed and should be reviewed as one coherent implementation:

- separate job and cache channels;
- complete-TU preparation in the first product version;
- DICT before LINES, with Need launched at `DICT_END` and Fill eligible at the next writer
  quantum;
- clone-independent immutable cache objects;
- standalone Asio confined to the new cache-I/O module;
- explicit automatic fallback to the established transfer;
- bounded per-relationship queues and one shared C-uplink budget.

The following are intentionally not frozen by this document:

- F namespace ownership: worker process, dedicated thread/context, or shared-context strand;
- whether F cache expands to a complete vector or writes bounded chunks directly to the compiler
  pipe;
- whether C prepares before requesting an F assignment;
- final frame quantum and queue ceilings;
- whether the optional Asio `io_uring` backend is ever worth enabling;
- which of P29, GRZ, or a later codec becomes the default Root/body codec.

## Names and lifetimes

| Name | Meaning | Lifetime |
|---|---|---|
| S | Icecream scheduler | cluster service |
| C clone | one `icecc` compiler-wrapper process | one compile invocation |
| C cpp child | preprocessor forked by a C clone | one TU |
| C cache | interner, ID allocator, Root builder, definition store, Fill builder | C `iceccd` lifetime |
| F clone | per-job handler forked by F `iceccd` | one compile invocation |
| F compiler | compiler forked by an F clone | one TU |
| F cache | Root/definition store, Need computation, expansion, job rendezvous | F `iceccd` lifetime |
| job connection | existing C-clone-to-F-clone TCP connection | one compile invocation |
| cache channel | persistent multiplexed C-cache-to-F-cache TCP connection | active C/F relationship |
| transaction | immutable Root and reconstruction recipe for one TU | cache-managed |
| attachment | one F clone consuming one transaction | one compile attempt |
| relationship | cache history for `(C_STORE_GUID, F_STORE_GUID)` | store-incarnation pair |

The letter F denotes a host/cache in the architecture and an F index in the simulator. An F may
offer many compile slots. A slot is not a separate cache.

The first deployment binds these names one-to-one: one submitting box owns one C cache authority,
one C GUID, and one physical uplink. A later scenario with several submitting boxes represents
independent authorities, GUIDs, uplinks, and F arenas. The simulator does not add a delegated
egress layer between a submitting box and its C cache.

## Existing Icecream path

Current Icecream already has useful compiler plumbing:

1. The C client obtains `UseCSMsg` and opens a direct job connection in
   [`client/remote.cpp`](../../client/remote.cpp#L411).
2. It forks the preprocessor, drains its pipe in 100,000-byte pieces, and sends each piece as
   `FileChunkMsg` in [`client/remote.cpp`](../../client/remote.cpp#L271) and
   [`client/remote.cpp`](../../client/remote.cpp#L536).
3. Protocol 44 frames messages with a four-byte length and limits an outer message to 1 MiB in
   [`services/comm.h`](../../services/comm.h#L38) and
   [`services/comm.cpp`](../../services/comm.cpp#L171).
4. Protocol 40 and later compress each `FileChunkMsg` independently with zstd in
   [`services/comm.cpp`](../../services/comm.cpp#L535) and
   [`services/comm.cpp`](../../services/comm.cpp#L2202).
5. F accepts the connection and forks a per-job handler in
   [`daemon/main.cpp`](../../daemon/main.cpp#L1585) and
   [`daemon/serve.cpp`](../../daemon/serve.cpp#L137).
6. The handler forks the compiler and writes incoming chunks into compiler stdin in
   [`daemon/workit.cpp`](../../daemon/workit.cpp#L124),
   [`daemon/workit.cpp`](../../daemon/workit.cpp#L345), and
   [`daemon/workit.cpp`](../../daemon/workit.cpp#L425).

Thus protocol 44 overlaps preprocessing, transfer, and compilation within a TU. Protocol 50's
first implementation deliberately trades that particular overlap for complete-TU interning and
factorization, while retaining concurrency across TUs.

## Current M5 boundary

The M5 capability program currently has this per-TU path:

```text
producer process
    -> pipe reader
    -> complete RawTU byte vector
    -> C interning and whole-TU factorization
    -> PreparedTU
    -> Root / Need / Fill
    -> F reconstruction
    -> compiler-verifier pipe
```

The reader calls `read_all` for the complete manifest length before publishing a `RawTU` in
[`capability/cap_m5_stream_main.cpp`](../cap_m5_stream_main.cpp#L1212). The interner publishes a
`PreparedTU` in [`capability/cap_m5_stream_main.cpp`](../cap_m5_stream_main.cpp#L1297).

The boundary is per TU, not global:

```text
time ------------------------------------------------------------------>

TU0  [preprocess/read] [intern] [Root/Need/Fill] [expand] [compile]
TU1       [preprocess/read] [intern] [Root/Need/Fill] [expand] [compile]
TU2            [preprocess/read] [intern] [Root/Need/Fill] [expand] [compile]
```

M5 reconstructs into a byte vector before its compiler writer begins. That is the initial
product path as well; a bounded expander is a later local optimization.

## Product process map

```mermaid
flowchart LR
  subgraph CH["C host"]
    BUILD["build tool"] --> CC["C clone - one TU"]
    CC -->|fork| CPP["preprocessor"]
    CPP -->|raw pipe| CC
    CC <-->|PREPARE / ROUTE / DETACH| CSTORE["persistent C cache"]
    CICE["C iceccd"] <-->|assignment| CC
    CICE -->|starts and watches| CSTORE
  end

  S["scheduler"]

  subgraph FH["selected F host"]
    FICE["F iceccd"] -->|fork per job| FC["F clone"]
    FICE -->|starts and watches| FSTORE["persistent F cache"]
    FC <-->|ATTACH / READY / TAKE_INPUT| FSTORE
    FC -->|fork and stdin pipe| COMP["compiler"]
  end

  CICE <-->|GetCS / UseCS| S
  S <-->|slots / job status| FICE
  CC <-->|job TCP: CompileFile cache reference and result| FC
  CSTORE <-->|persistent cache TCP: Root / Need / Fill| FSTORE
```

The job socket and cache channel serve different purposes:

```text
C clone <-> F clone: execute this compiler job; return diagnostics/object/result
C cache <-> F cache: make this immutable input transaction available
F clone <-> F cache: attach this compiler process to that transaction
```

The main `iceccd` event loops do not perform interning, compression, expansion, or bulk copying.
The cache services use bounded worker pools. One I/O thread may own a cache socket initially;
encoding and decoding work need not run on that thread.

## Concrete daemon and client integration

The product implementation should be a sidecar service owned by `iceccd`, not a large codec
subsystem inserted into `daemon/main.cpp`. One installed executable can host the C-authority role,
the F-store role, or both roles on a machine that submits and receives work:

```text
iceccd
  starts/watches icecc-cache-service
  owns the existing public TCP listener
  passes cache-session descriptors to the sidecar

icecc-cache-service
  C role: local PREPARE/ROUTE sessions + outbound channels to F caches
  F role: inbound channels from C caches + local ATTACH sessions from F clones
  shared: interner/codec implementation and immutable object types
```

The C and F roles receive distinct store GUIDs even when they live in one sidecar process. Their
stores and counters are separate. The sidecar exposes one Unix socket under the daemon runtime
directory; the first local command selects C-role preparation or F-role attachment.

### Reuse the existing daemon TCP endpoint

The smallest scheduler change is no scheduler change. `UseCSMsg` already gives the C clone the
selected daemon address and port. The C clone passes that endpoint to its local C cache in
`ROUTE`. C cache opens a second connection to the same F daemon port, performs ordinary protocol
version negotiation, and sends a new `CACHE_SESSION(C_STORE_GUID, profile offer)` as the first
message. `daemon/main.cpp` classifies that connection and passes the connected descriptor plus
the decoded bootstrap fields to the F cache sidecar. The sidecar then owns all subsequent cache
frames on that descriptor.

```text
C clone ---- existing connection ----> F daemon/job child
   |
   +-- ROUTE(F address, port) --> C cache ---- second persistent connection ----> F daemon
                                                                              |
                                                                              +-> F cache
```

This avoids one cache TCP connection per job. C cache reuses the relationship for every later TU
assigned to that F-store incarnation. The F cache answers SESSION with `F_STORE_GUID` and the
selected component-codec profile. If the daemon restarts at the same address, the new F GUID makes
the relationship cold without changing routing keys elsewhere.

Descriptor handoff requires a mandatory bootstrap barrier because current `MsgChannel::read_a_bit`
may read ahead into its private input buffer. C cache sends only `CACHE_SESSION` and does not send
the first cache frame until the F sidecar answers `SESSION_READY(F_STORE_GUID, selected profile)`.
This ensures no post-bootstrap bytes exist for `MsgChannel` to read ahead.

The daemon consumes the complete `CACHE_SESSION`, calls a new
`MsgChannel::release_fd_if_input_empty()`, and passes the descriptor plus bootstrap fields to the
sidecar over its Unix control socket. That method succeeds only when the negotiated message has
been fully consumed and `inofs == intogo`; it transfers descriptor ownership without closing it.
The sidecar assigns the descriptor to an Asio TCP socket and sends `SESSION_READY` using
`CacheChannel` framing. If the old channel reports buffered bytes, the handoff does not proceed
and C reconnects. Tests cover every split of the protocol/version and CACHE_SESSION bytes, and a
deliberately early first cache frame confirms that the clean-boundary check catches the case.

An alternative dedicated cache port would require returning that port through job or scheduler
metadata. Keep it as a later deployment option, not the first implementation.

### Extend `CompileFileMsg`, do not create a second job protocol

For protocol 50, add an input descriptor to `CompileFileMsg`:

```text
InputDescriptor {
    mode: LEGACY_CHUNKS | CACHE_REFERENCE
    if CACHE_REFERENCE:
        C_STORE_GUID
        TU_SEQ
        raw_length
}
```

For negotiated versions below 50, these fields are absent and serialization is byte-for-byte the
established format. For version 50 with `LEGACY_CHUNKS`, the receiver enters its existing
`FileChunkMsg` loop. For `CACHE_REFERENCE`, the F job child creates the compiler exactly as it
does today but obtains stdin bytes from the local F cache attachment instead of the job socket.
Diagnostics, object chunks, statistics, and `CompileResultMsg` remain on the job socket.

The C clone receives a local preparation handle at `PREPARE_ACCEPTED` and learns
`C_STORE_GUID/TU_SEQ` when `PREPARED` publishes the immutable result. It does not need to know the
F GUID. The C cache learns F GUID through SESSION and internally binds the route. This removes a
needless clone round trip and makes daemon restart handling a cache-channel concern.

Do not duplicate the compiler process loop in `daemon/workit.cpp`. Extract its input side behind
a small readiness-oriented source:

```text
CompilerInputSource
  native_fd()                    // included in the existing poll set
  read_ready_chunk()             // bytes, EOF, or input error
  uncompressed_bytes()
  physical_input_bytes()

LegacyChunkSource               // adapts FileChunkMsg from the job MsgChannel
CacheAttachmentSource           // adapts INPUT_DATA/INPUT_END from the local F-cache session
```

The existing stdout, stderr, child-exit, and result logic remains one loop. Only the source of
stdin chunks changes. The later direct-pipe mode is a third source implementation whose stdin
writer lives in F cache; it should not create a third compiler lifecycle.

Likewise, keep one C-side preprocessor pump. Its sink is selected before bytes flow:

```text
LegacyRemoteSink                // existing FileChunkMsg sender
CachePrepareSink                // PREPARE_DATA to local C cache
```

An already prepared retry obtains an exact local byte source from `TAKE_LEGACY_INPUT` and feeds
`LegacyRemoteSink`; it does not invoke the preprocessor pump a second time.

### Local session placement

C clones are external processes, so they connect to the sidecar's Unix listener. F job children
are forked by `iceccd`; they should also use the same local protocol rather than inheriting a
mutable cache connection across fork. Each local session has one owner and one transaction:

```text
C clone session: PREPARE_BEGIN -> DATA* -> PREPARE_END -> PREPARED -> ROUTE/DETACH
F clone session: ATTACH -> WAIT_READY -> TAKE_INPUT/DATA* -> DETACH
```

If later measurement favors direct compiler-pipe writing, F clone passes the pipe descriptor in
`TAKE_INPUT_TO_FD`; the transaction and attachment protocol remain the same. Shared memory is
likewise a local-session optimization and does not change cache-channel messages.

### Sidecar lifecycle

`iceccd` starts the sidecar before accepting protocol-50 cache sessions and retains a small
control socket for status and descriptor handoff. Startup produces C/F GUIDs and publishes the
local Unix endpoint. If startup fails, `auto` mode continues through the established transfer.

The daemon watches the sidecar PID. A replacement sidecar receives new GUIDs and begins cold.
Existing ordinary compile jobs remain governed by the existing daemon child lifecycle. Cache-
reference jobs whose F attachment disappeared end that attempt and may be retried through the
normal client policy.

On orderly daemon exit, it stops new cache sessions, asks the sidecar to drain bounded completion
traffic, and then joins it. The sidecar never becomes the scheduler authority and does not alter
slot counts; it only makes input bytes available to already-assigned jobs.

## Cache-service execution model

This section deliberately separates three choices that were previously mixed together:

1. **I/O API:** how sockets, timers, and cancellation are driven;
2. **namespace ownership:** whether one C GUID lives in a process, a dedicated thread, or a
   strand in a shared process;
3. **CPU execution:** where interning, compression, Fill construction, expansion, and eviction
   run.

The I/O decision can be made now without freezing the namespace decision. The leading I/O choice
is standalone Asio on C++23. The F-cache-per-C-cache process/thread/strand choice remains an
explicit review item and must be decided by measurement before the product runtime is frozen.

### Runtime invariants independent of the threading choice

Every implementation option must preserve the following ownership rules:

- one cache socket has exactly one reader and exactly one writer state machine;
- one `C_STORE_GUID` owns one logically independent F namespace;
- no operation on namespace A waits synchronously for namespace B;
- socket callbacks do bounded bookkeeping and never perform a whole-TU codec operation;
- codec workers publish immutable results back to the socket/namespace owner;
- every outbound relationship has bounded queues and a reserved Fill/control budget;
- all transaction state is addressed by `(C_STORE_GUID, TU_SEQ)` and survives clone detach;
- shutting down one namespace cannot invalidate buffers still referenced by another namespace;
- the protocol and stored representation do not depend on which runtime option is selected.

These rules are the seam between the protocol and the runtime. They allow the simulator, codec,
wire tests, and most cache-state tests to remain unchanged if the runtime choice changes.

### Leading I/O choice: standalone Asio on C++23

Build the new cache services on C++23 and standalone Asio. Asio supplies nonblocking TCP and Unix
sockets, timers, scatter/gather buffers, executor ownership, strands, and coroutine integration
while using `epoll` on ordinary Linux builds. The protocol still owns buffer lifetime, byte
limits, priority, admission, and queue policy; Asio owns only readiness and completion plumbing.

Use the non-Boost distribution with `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`, and preferably
`ASIO_SEPARATE_COMPILATION` in a new cache-I/O library. Keep Asio types private to that library:
the interner, codec, wire-value types, simulator, scheduler, and existing job path should depend
on plain value interfaces rather than Asio headers. There is no standard `std::asio` socket
library in C++23, so moving the cache module to C++23 does not remove the need for standalone
Asio.

```text
cache responsibility             standalone-Asio primitive
--------------------             -------------------------
one or more I/O executors        asio::io_context
owned relationship state        tcp::socket + strand/executor
framed reader                    awaitable reader loop
single queued writer             awaitable writer loop
codec CPU work                   bounded worker executor/pool
reconnect and retention clocks   steady_timer
clone/cache local sessions       local::stream_protocol::socket
frame scatter/gather             const_buffer sequence
```

There is exactly one outstanding write operation per relationship. Producers enqueue immutable,
reference-counted frame buffers; the writer selects the next bounded quantum and advances a
partial-write cursor until that quantum completes. The reader owns the incremental frame parser.
Neither coroutine touches the codec's mutable state directly: decoded commands are posted to the
namespace executor, and completed encoded buffers are posted back to the relationship executor.

The initial Linux backend is Asio's normal `epoll` implementation. Old kernels therefore remain
usable. Direct `io_uring` code is not part of the first implementation. An Asio `io_uring` build
may be benchmarked later from the same protocol sources, but it is a replaceable backend rather
than an architectural requirement.

C++23 is useful independently of Asio: `std::span`, `std::expected`, `std::jthread`,
`std::stop_token`, endian/byteswap support, and standard coroutines make ownership and shutdown
paths explicit. Do not migrate the scheduler or protocol-44 job loops merely to adopt these
facilities. The new cache modules can use C++23 while the established path remains intact.

### C-cache runtime

One C cache usually talks to tens of Fs. The leading C implementation therefore uses:

```text
C clone local sessions -----------+
preprocessor buffers -------------+--> bounded codec workers
Need/ACK decode ------------------+             |
                                                  v
                                    immutable Root/Fill buffers
                                                  |
                       +--------------------------+--------------------------+
                       v                          v                          v
                 relationship Fa            relationship Fb            relationship Fc
                 priority queues            priority queues            priority queues
                       \__________________________|__________________________/
                                                  v
                                     one Asio I/O context initially
```

Start with one I/O thread because the expected active relationship count is small and socket
work is bounded. The ownership interface must be shardable without changing protocol state:

```text
io_owner = stable_hash(F_STORE_GUID) % io_context_count
```

If one I/O core becomes limiting, two or four contexts may own disjoint relationships. A socket
never migrates while it has pending operations. Codec work is shared across relationships, so a
large Fill for one F can use available CPU workers without allocating a permanent worker thread
to that F.

### F-cache namespace ownership: open implementation decision

An F receives cache channels from many Cs and clone attachments for many concurrent jobs. Data
under different C GUIDs is logically disjoint, but that fact does not by itself determine whether
the boundary should be a process, a thread, or an Asio strand. Keep the following three layouts
buildable behind one `NamespaceRuntime` interface until the complete-path measurements settle it.

| Option | Shape | Advantages | Costs and questions |
|---|---|---|---|
| A: worker process per C GUID | supervisor routes cache and local-attachment descriptors to a lazily started worker process | clearest lifetime; whole namespace reclaimed on exit; a stalled codec worker affects one C | process and IPC overhead; one small worker pool per active C unless CPU work is delegated |
| B: I/O thread/context per C GUID | one F-cache process; each active C GUID owns one thread, `io_context`, channel, clone sessions, timers, and namespace | direct local routing; simple sequential namespace ownership; slow C has a dedicated I/O thread | idle thread/stack per active C; CPU work must still be bounded; namespace teardown must release all heap state explicitly |
| C: shared I/O contexts plus one strand per C GUID | a small process-wide I/O pool; each GUID owns a strand and bounded queues | lowest idle overhead; shared workers; scales to many mostly idle Cs | requires disciplined quotas so one namespace cannot monopolize callbacks or worker completions |

Option B—the threaded F-cache-per-C-cache layout—remains specifically open for bigoracle review.
It may be the simplest first implementation if the normal number of active Cs is around sixteen:
sixteen mostly sleeping I/O threads are inexpensive, every namespace has an obvious sequential
owner, and old-kernel behavior is straightforward. Option A remains attractive when whole-heap
reclamation materially simplifies long-lived cache rotation. Option C is the likely consolidation
target if process/thread overhead becomes visible.

Do not allow these alternatives to produce three protocol implementations. Define:

```text
NamespaceRuntime
  accept_cache_channel(C_STORE_GUID, connected_socket)
  attach_clone(C_STORE_GUID, TU_SEQ, local_socket)
  post_decoded(CacheCommand)
  post_codec_completion(CodecCompletion)
  begin_idle_retention(deadline)
  retire_when_unreferenced()
```

The supervisor or shared service reads only enough local/session metadata to choose a C GUID and
then hands the descriptor and command to the matching runtime. Root, Need, Fill, expansion, and
compiler-input bytes must not be copied through the supervisor after routing.

For Option A, start workers with a separate executable or `posix_spawn`, then initialize Asio and
threads inside the new process. For Option B, create the `io_context` and its thread lazily when
the first channel or attachment arrives. For Option C, create a strand lazily on the shared
executor. In all three cases, a TCP close starts an idle timer rather than immediately discarding
the namespace; a same-GUID reconnect reuses retained state.

### Coroutine and queue structure

Each cache relationship is two long-lived coroutines plus bounded queues:

```text
reader coroutine
  async_read outer header
  validate declared length and frame class
  async_read remaining frame
  decode fixed fields
  post command to NamespaceRuntime
  repeat

writer coroutine
  wait until any priority queue is nonempty
  choose next frame quantum
  async_write_some immutable buffers
  retain partial cursor across suspension
  record exact socket bytes
  complete delivery bookkeeping
  repeat
```

The queue classes are, in order:

1. Fill, acknowledgement, Ready, and reconnect progress;
2. continuation required to reach an already-started `DICT_END`;
3. bounded LINES continuation and new DICT work under deficit round-robin;
4. maintenance traffic that is not required by an attached job.

Once a frame quantum is submitted, it is not interrupted. Fill becomes eligible at the next
quantum. This is the exact implementation counterpart of the simulator's preemption boundary.

Each relationship has independent byte and item ceilings. The process also has a global encoded-
buffer ceiling. Crossing a relationship's normal ceiling pauses new DICT/LINES admission for
that destination but retains reserved capacity for Fill and completion. Crossing the global
ceiling pauses local preparation before allocating another complete output buffer. No callback
waits while holding a namespace or store lock; it suspends or returns to the executor.

### Codec workers and immutable buffer ownership

The I/O runtime must not own codec algorithms. It submits typed work items to a bounded pool:

```text
PrepareRawTU -> PreparedRoot
DecodeDict   -> DependencyManifest
EncodeFill   -> immutable EncodedFrameVector
InstallFill  -> InstalledDefinitionBatch
ExpandInput  -> bounded output chunks or complete RawTU
Evict        -> released object IDs and byte counts
```

Inputs are immutable spans backed by reference-counted slabs. Results become immutable before
they are posted to the namespace owner. A relationship queue holds references, not copied
vectors. The owner releases a frame only after the writer records its complete socket delivery
or after the relationship is retired. Retained replay buffers are charged separately from active
queue bytes.

The first implementation may expand a TU into one complete output vector because the codec
capability already verifies that boundary. The interface should nevertheless expose a chunk sink
so bounded expansion can be substituted later without changing Root/Need/Fill state.

### Shutdown and retention ordering

Runtime shutdown is an ordered state transition, not a thread cancellation shortcut:

1. stop admitting new local prepare/attach requests;
2. stop starting new DICT/LINES work while allowing queued Fill and completion messages to drain
   up to a configured deadline;
3. detach compiler consumers and close their local sessions;
4. close relationship sockets and cancel timers;
5. wait for posted codec completions to return or mark their immutable results unreferenced;
6. release transaction, replay, and namespace objects;
7. stop the I/O context and join owned threads, or exit the worker process.

A cache-channel disconnect does not execute this sequence by itself. It changes the relationship
to `disconnected-retained`, keeps complete objects, and starts reconnect/idle-retention timers.

## Identifiers

| Identifier | Scope | Purpose |
|---|---|---|
| `C_STORE_GUID` | one C-store incarnation | namespaces all C definitions and transactions |
| `F_STORE_GUID` | one F-store incarnation | identifies the actual destination cache incarnation |
| `TU_SEQ` | contiguous PreparedTU admission order under one C GUID | names one immutable prepared TU |
| `REL_SEQ` | contiguous projection under one `(C GUID, F arena)` | orders transfer and route-state commit |
| `ID64` | one immutable definition under a C GUID | stable shared-cache key |
| `LOCAL32` | one Root only | dense array index used during expansion |
| `DELIVERY_ID` | one cache relationship | names one Fill batch for acknowledgement/replay |

The proposed definition layout remains `GEN10 | SEQ54`. Sequence bits are never reused during
the C-store lifetime. `LOCAL32` remains dense and private to one TU. The full definition identity
is `(C_STORE_GUID, ID64)`.

The C clone passes the assigned daemon endpoint to its local C cache in `ROUTE`. The persistent
cache-channel SESSION returns `F_STORE_GUID`; the C cache owns that binding and detects a changed
incarnation. Neither clone needs to carry F GUID in the job reference. The job reference is
simply `(C_STORE_GUID, TU_SEQ)`.

## End-to-end interaction

### 1. Allocate and prepare the TU on C

`PREPARE_BEGIN` creates a local preparation handle. `TU_SEQ` is allocated when the complete,
immutable PreparedTU is admitted to the C-global catalogue:

1. C clone opens a bounded local session and sends `PREPARE_BEGIN`; the session or a local-only
   handle identifies the in-progress preparation.
2. C clone forks the normal preprocessor and drains its pipe into `PREPARE_DATA` messages.
3. The first version assembles one complete raw TU before interning and factorization.
4. C cache interns lines/regions, assigns immutable ID64 values, selects superblocks, emits
   inline material, and constructs a dense `LOCAL32 -> ID64` map.
5. C cache allocates the next `TU_SEQ`, atomically publishes `PreparedTu`, and replies
   `PREPARED(C_STORE_GUID, TU_SEQ)`.
6. The prepared queue is bounded by both bytes and TU count. Build parallelism and that queue,
   rather than the number of remote slots alone, bound memory.

Allocation does not publish a partial transaction. Before `PREPARED`, the key may exist only as
a local preparation placeholder and an F attachment waiter. DICT transmission begins only after
the immutable PreparedTu is published.

The PreparedTu is independent of its destination. Two client orderings fit the same protocol:

```text
minimal first patch:
  GetCS/UseCS -> establish cache SESSION -> PREPARE_BEGIN -> preprocess/prepare/TU_SEQ
  -> ROUTE -> send cache job reference -> cache transfer

later measured option:
  PREPARE_BEGIN -> preprocess/prepare -> GetCS/UseCS -> establish cache SESSION
  -> ROUTE + cache job reference
```

The first ordering stays closest to `client/remote.cpp`, which currently obtains an assignment
and sends `CompileFileMsg` before starting the preprocessor. It holds an F slot during complete-TU
preparation. The second avoids that slot wait but changes client/scheduler timing. Keep the order
behind one client sequencing seam and measure it rather than encoding it in cache identity.

### 2. Bind the job and cache relationship

After scheduler assignment, C clone establishes the normal job connection and already has the F
daemon endpoint from `UseCSMsg`. Once `TU_SEQ` has been allocated, it performs two independent
operations; `ROUTE` may remain pending until `PREPARED` publishes the Root:

```text
C clone -> C cache: ROUTE(TU_SEQ, F daemon endpoint)
C clone -> F clone: CompileFileMsg(CACHE_REFERENCE, C_STORE_GUID, TU_SEQ, compile metadata)
```

C cache opens or reuses the persistent channel for that endpoint and binds the returned F GUID.
F clone performs:

```text
F clone -> F cache: ATTACH(C_STORE_GUID, TU_SEQ, compiler-job handle)
```

Either the Root or attachment may arrive first. F cache stores one flag for each and proceeds
when the required state exists. No polling between clones is required.

### 3. Send DICT, then stream LINES

Root is one logical transaction split at the earliest useful dependency boundary:

| Component | Meaning |
|---|---|
| DICT header | C GUID, TU sequence, relationship sequence, generation, raw length, component counts |
| DICT used map | dense `LOCAL32 -> ID64` vector containing every shared dependency |
| DICT block metadata | immutable identifiers needed to interpret the body token stream |
| LINES body | ordered local32/superblock token stream |
| LINES inline material | one-use text and compact offset/length/value columns |

C cache sends `DICT_BEGIN`, the complete DICT, and `DICT_END`, then continues with LINES frames.
F can determine the exact missing ID64 set at `DICT_END`; it does not wait for `LINES_END`.
Because the cache channel is full duplex, F sends Need in the reverse direction while C continues
transmitting this TU's LINES. Other F relationships may progress concurrently through the shared
C-uplink scheduler.

```text
time -------------------------------------------------------------------->

C -> F:  DICT(343) | LINES(343)-1 | LINES(343)-2 | ... | LINES_END(343)
F -> C:                 NEED(343) ------>
C CPU:                                  build FILL(343)
C -> F:                                             FILL at next queue quantum
```

Frames are ordered within each DICT or LINES component. Frames for other F relationships may
appear between this relationship's writer quanta, but the next TU on this relationship starts
only after the current transaction commits. `DICT_END` is the Need barrier. `LINES_END`, all
dependencies resident, and a live job attachment are the three conditions for compiler input
readiness.

Root is self-describing with explicit IDs. Cross-F completion order has no semantic meaning.
Within one F arena, route-local codec history advances exactly at committed `REL_SEQ`; globally
reusable learned artifacts remain immutable, named definitions or model checkpoints.

### 4. Compute Need

At `DICT_END`, F cache resolves each used ID64 against its persistent store and records the
transaction's full dependency set while LINES continues arriving. Its semantic response is:

```text
NEED(C_STORE_GUID, TU_SEQ, sorted unique missing ID64 set)
```

Need sets from different F relationships may overlap. The persistent channels deliver all of
them to the one C cache that owns the definitions, while each F receives the objects missing from
its own arena.

### 5. Materialize and send Fill

C cache builds Fill from the active transaction's Need. Fill construction overlaps the remaining
LINES transfer. As soon as an encoded Fill buffer is ready, it enters the relationship's
Fill-priority queue and is submitted at the next bounded writer quantum; already-submitted bytes
cannot be overtaken. Each completed definition is installed once in that F arena and is then
available to later `REL_SEQ` values:

```text
FILL(DELIVERY_ID,
     sorted/delta-coded ID64 values,
     value/category column,
     offset column,
     length column with long-length escape,
     concatenated text,
     required path/index columns)
```

Fill is not owned by a compiler clone or permanently tied to the transaction whose Need first
mentioned it. F cache installs complete definitions idempotently and sends:

```text
FILL_APPLIED(DELIVERY_ID)
```

C cache retains an unacknowledged Fill buffer. If the cache channel reconnects, it may replay
that batch. A duplicate installation is a no-op. Historical "sent" state never overrides a new
Need because F may have evicted the object; only an active unacknowledged delivery is coalesced.

### 6. Mark input ready and attach the compiler

Every transaction maintains a count of unresolved IDs plus `lines_complete` and attachment
state. Installing a Fill updates all affected transactions. When the dependency count is zero
and `LINES_END` has arrived, F cache marks the immutable transaction cache-ready and notifies
both its local attachment and C cache:

```text
F cache -> F clone: INPUT_READY(C_STORE_GUID, TU_SEQ)
F cache -> C cache: CACHE_READY(C_STORE_GUID, TU_SEQ)
```

F clone requests `TAKE_INPUT`. The initial implementation expands into a complete vector and
passes bounded chunks over the local Unix session to the F clone, which writes compiler stdin.
If measurement shows that local copy costs more than 10% of the complete path, F clone passes
the compiler-pipe descriptor to F cache and F cache writes expansion chunks directly.

The compiler result, diagnostics, and object file continue over the normal job connection.

## Concurrency example

Suppose global admission is `A, B, C, D`, routing projects it as:

```text
Fa: A(REL_SEQ 0), C(REL_SEQ 1)
Fb: B(REL_SEQ 0), D(REL_SEQ 1)
```

At time zero, A and B may be the active transaction on their independent relationships. Their
writer quanta share the one physical C uplink. Each F emits Need as soon as its active DICT
completes, regardless of whether that TU's LINES has completed. Fill takes the next permitted
quantum on that relationship. If B commits first, D may begin while A is still transferring:

```text
Fa: A [DICT -> Need -> LINES/Fill -> commit] -> C [starts]
Fb: B [DICT -> Need -> LINES/Fill -> commit] -> D [starts]
                         |                         |
                         +-- B compiler continues while D transfers
```

Cross-F completion may reorder. Fa's state nevertheless advances only A then C, and Fb's state
only B then D. Objects installed for A remain available when C computes Need. The same object may
still cross again to Fb because its arena is independent.

The cache-channel sender uses a bounded frame scheduler, initially:

1. Fill and completion/control traffic;
2. continuation frames required to reach an already-started `DICT_END`;
3. bounded LINES continuation quanta and relationship-head DICT starts under deficit round-robin;
4. unrelated bulk cache work.

A DICT is kept compact and normally completed without interleaving so F can launch Need quickly.
Once DICT ends, LINES is preemptible at the configured frame quantum. A Fill that becomes ready
while LINES is being sent takes the next C-to-F quantum. This overlaps reverse-direction Need and
Fill construction with LINES but does not pretend that two C-to-F payloads occupy the same link
at the same instant.

Round-robin within a class prevents a large TU from occupying the channel indefinitely. Frame
quantum is a measured parameter, not part of codec semantics. Start with the existing roughly
100 KiB scale and compare 64, 128, 256, and near-1-MiB frames.

One persistent full-duplex socket per relationship is the first implementation. The C-uplink
scheduler multiplexes these relationship sockets while each socket carries one active dialogue.
Multiple transport lanes per relationship remain a measured later option only if a singleton
dialogue leaves the physical link idle; they do not alter transaction identity or cache state.

## Clone-independent lifetime

Cache transaction and job attachment are separate state machines:

```text
cache transaction:
    receiving Root -> waiting for definitions -> cache ready -> retained -> normal eviction

job attachment:
    waiting for transaction -> attached -> consuming input -> detached

materialized raw bytes:
    absent -> expanded for consumer -> consumed -> discarded
```

Therefore:

- C clone ending detaches its compile attempt; C cache may complete the cache transfer.
- F clone ending detaches one consumer; F cache retains Root and installed definitions.
- A replacement F clone on the same F can attach to the same `(C_GUID, TU_SEQ)`.
- A retry assigned to another F can route the same prepared transaction to that F's cache.
- A job cancellation does not delete cache content.
- Reconstructed raw bytes and compiler pipes are job-local and disposable.

If a complete Root can become the basis of later coding, it receives an immutable ID64 like any
other reusable object. Later Roots reference it explicitly. F may eventually evict it under the
normal policy, after which a future Need restores it.

## Cache-channel interruption and restart

The one-dialogue relationship deliberately makes replay a one-cursor operation:

1. Every PreparedTU, Root, and complete Fill object has a stable identity and digest.
2. F retains `next_REL_SEQ`, the committed route-state digest, installed immutable objects, and
   an arena epoch for each C GUID.
3. C retains the current encoded transaction until its relationship commit is acknowledged.
4. On reconnect, F returns its arena epoch, `next_REL_SEQ`, and route-state digest. If they match
   C's retained committed state, C replays at most the one current transaction from its start.
5. Complete immutable objects installed before interruption remain present; partial frames and
   the incomplete active overlay are discarded. F recomputes Need from its actual object set.
6. A restarted F cache, or an explicitly discarded arena, reports a new epoch and begins cold.
7. A restarted C cache uses a new C GUID. Old F entries remain separate until ordinary eviction.

Only one live session epoch may advance a relationship. Repeating the same `REL_SEQ` with the
same transaction digest has the same effect; a different digest for that cursor is an exactness
error. No compiler clone owns cache-channel progress.

## Many Fs and many Cs

One C has one authoritative definition store and one relationship channel per active F:

```text
C GUID G
  -> Fa GUID: Root/Need/Fill for TUs 1, 6, 7, 9, ...
  -> Fb GUID: Root/Need/Fill for TUs 2, 4, 10, ...
  -> Fc GUID: Root/Need/Fill for TUs 3, 5, 8, ...
```

Each F retains the subset it has learned. Cold definition bytes are therefore replicated once
per destination F that actually needs them. Scheduler affinity reduces this replication when
load permits, but round-robin placement remains valid.

With sixteen independent submitting boxes, an F cache partitions state by C GUID:

```text
F cache
  C GUID G0 -> Roots and definitions
  C GUID G1 -> Roots and definitions
  ...
  C GUID G15 -> Roots and definitions
```

At most one persistent channel is needed for each active GUID pair, not one per compiler slot.
Each submitting box retains its own authority, GUID, and uplink; the first simulator does not
combine them behind a shared logical authority.

## Topology, bandwidth, and launch semantics

Topology notation must state where every capacity applies. Use these fields in scenario JSON and
human-readable names:

| Field | Meaning |
|---|---|
| `c_count` | number of independent submitting boxes/C authorities/GUIDs/uplinks |
| `f_count` | number of F hosts/caches |
| `slots_per_f` | simultaneous compiler jobs available on each F, not across all Fs |
| `c_uplink_bps` | aggregate C-to-network limit for one C across all of its F relationships |
| `f_ingress_bps` | aggregate receive limit on one F across all Cs |
| `route_bps` | optional ceiling for one `(C,F)` relationship; defaults to no lower than the endpoint limits |
| `fabric_bps` | optional aggregate shared-fabric limit across all active relationships |

The concise grammar is:

```text
C<c-store-count>F<f-count>_<slots-per-F>B<shared-bandwidth-per-C>
```

For example:

```text
C1F20_200B1G
```

means one C, twenty Fs, 200 compile slots on **each** F (4,000 slots total), and one shared
1-Gbit/s outgoing limit at C. It does not grant 1 Gbit/s independently to all twenty routes.
If a test intentionally grants per-route bandwidth, say so explicitly, for example
`C1F20_200B20G_R1G`. Optional nondefault suffixes are `I` for each F's ingress ceiling,
`R` for each C/F route ceiling, and `X` for the shared fabric ceiling.

`C1` means exactly one submitting box, C authority, GUID, and shared C uplink. `C16` means sixteen
independent instances of that unit. Each has its own configured C-uplink capacity; F ingress and
the shared fabric may still couple their physical progress.

Two large-capacity controls need careful interpretation:

```text
C1F1_1000000B1G
C1F10_100000B1G
```

Both expose one million nominal compile slots and the same shared 1-Gbit/s C uplink. The second
is not automatically faster. When compile slots are already non-limiting, it has the same source
bandwidth ceiling and may transmit more cold Fill because definitions are spread across ten F
caches. It becomes network-faster only if the model grants separate route bandwidth that is not
bounded by C's uplink. These controls are useful for separating network and compile limits, but
they are not the primary deployment topology.

### What one C means

One C is one source host and one authoritative cache, not one serial compile process. A build may
run many C clones and preprocessor children concurrently. The scheduler may assign their TUs to
many Fs. The simulator and product implementation therefore admit globally without a per-C
stop-and-wait queue, then use one ordered active dialogue independently on each F relationship:

```text
one C host
  C clone 17 -> preprocess TU17 -> prepare -> route Fa
  C clone 18 -> preprocess TU18 -> prepare -> route Fc
  C clone 19 -> preprocess TU19 -> prepare -> route Fb
  ... Fa/Fb/Fc heads overlap subject to the one C uplink and bounded buffers
```

TU release is governed by the measured producer/preprocessor readiness trace and explicit local
capacity, not by an arbitrary launch interval. When a primary C1 scenario starts, it launches all
work the build makes ready; there is no inserted delay between clone launches.

Repeated-build research scenarios also use zero artificial inter-build gap by default. Build
`n+1` begins as soon as the configured build driver permits after build `n`. A gap is present only
when the scenario explicitly tests retention/eviction behavior. Reports show active-generation
elapsed regardless, but removing artificial gaps is still important because time-based cache
rotation and eviction observe wall-clock time.

### Primary topology matrix

The first product/simulator acceptance matrix should include:

| Purpose | Topology |
|---|---|
| single-destination byte/timing reference | `C1F1_1000000B1G` |
| large-slot 1-Gbit/s and 10-Gbit/s controls | `C1F1_10000B1G`, `C1F1_10000B10G` |
| active-F width sweep, 200 slots/F, one shared 1-Gbit/s uplink | `C1F1_200B1G`, `C1F2_200B1G`, `C1F3_200B1G`, `C1F4_200B1G`, `C1F20_200B1G` |
| same active-F width sweep with faster C source | replace `B1G` with `B10G` |
| independent-C contention | `C16F20_200B1G`, with explicit F ingress and fabric limits |
| cache-distribution comparison | width sweep under round-robin and bounded sticky-frontier placement |

Every result header must print the expanded capacities, not only the concise name. Otherwise a
per-route 1-Gbit run and a shared-uplink 1-Gbit run are too easy to confuse.

## Generation rotation and eviction

The active C generation is append-only. The launch policy remains:

1. consider rotation after roughly eight hours;
2. flip at a quiet TU boundary, preferably after one minute without new intake;
3. retain the old read-only generation for three hours so delayed Need can still be filled;
4. use the new generation for subsequent TUs;
5. reclaim the old generation after retention and after local prepared references are gone.

F uses a bounded LRU or ARC-like policy across C namespaces. An entire source namespace may be
removed after roughly two hours idle. Eviction never changes meaning: a later Root names the
same immutable ID and produces another Need.

Active expansion objects are pinned only while an output chunk uses them. Retained Roots and
definitions are otherwise independently evictable because missing referenced objects can be
requested again.

## Protocol-50 messages

Protocol negotiation already exists. Protocol 50 adds a cache-input mode to `CompileFileMsg`
and a persistent cache-session connection accepted by F `iceccd` and handed to F cache.

### Job connection

```text
CompileFileMsg {
    normal compile/environment metadata
    InputDescriptor {
        mode = CACHE_REFERENCE
        C_STORE_GUID
        TU_SEQ
        advertised raw length
    }
}

COMPILE_RESULT
diagnostics and object data
```

### Cache channel

Keep existing outer framing:

```text
u32 network-order outer_message_length
u32 Msg::CACHE_FRAME
cache-frame payload
```

The compact inner envelope remains compatible with the proposed three-bit type:

```text
u32 type_and_length = (type3 << 29) | payload_length29
message-specific identifiers
payload
```

| type3 | Class | Examples |
|---:|---|---|
| 0 | SESSION | HELLO, GUID pair, arena epoch, codec capabilities, `next_REL_SEQ`, route-state digest |
| 1 | ROOT | `TU_SEQ/REL_SEQ`, DICT/DICT_END, and LINES/LINES_END for the active transaction |
| 2 | NEED | TU key and missing ID64 set |
| 3 | FILL | delivery ID and immutable definitions |
| 4 | ACK | Fill applied and frame/batch progress |
| 5 | READY | cache transaction reconstructable |
| 6 | CONTROL | detach, retire, reconnect/resume controls |
| 7 | reserved | later extension |

The existing 1 MiB outer ceiling remains. Logical DICT, LINES, and Fill objects span as many
frames as needed. Components carry an encoding selector (`raw`, `zstd-1`, or `zstd-3`) and raw/encoded
lengths. Already encoded component bytes are not passed through `FileChunkMsg` compression.

Zstd-6-long and zstd-19-long remain whole-corpus comparison controls, not live per-frame
defaults. P29, GRZ, and later codecs plug into Root/Fill component boundaries; none defines a
different job or cache protocol.

### Local cache APIs

```text
C clone <-> C cache:
    ENSURE_ROUTE(F endpoint) / SESSION_READY(F GUID, profile)
    PREPARE_BEGIN / PREPARE_ACCEPTED(local preparation handle)
    PREPARE_DATA* / PREPARE_END / PREPARED(C GUID, TU sequence)
    ROUTE(TU sequence, F endpoint)
    TAKE_LEGACY_INPUT / RAW_DATA* / RAW_END
    DETACH

F clone <-> F cache:
    ATTACH
    WAIT_READY / INPUT_READY
    TAKE_INPUT / INPUT_DATA* / INPUT_END
    optional TAKE_INPUT_TO_FD
    DETACH
```

Use Unix sockets first. Shared-memory transport or descriptor passing is a replaceable local
optimization, selected only if the end-to-end measurement justifies it.

## Fallback and feature selection

The existing per-job transfer remains available for the lifetime of protocol 50. The fallback
is not a second cache protocol: it is the established `FileChunkMsg` path, including its current
per-chunk compression and direct compiler-pipe behavior.

### Selection modes

Expose one operator setting on C:

| Mode | Behavior |
|---|---|
| `legacy` | always use the established per-job input transfer |
| `auto` | use cache input only after the assigned F and its cache channel are ready; otherwise use the established transfer |
| `cache` | require cache input; report an attempt failure if it cannot be established; intended for development and acceptance runs |

`auto` is the eventual default. Protocol-version negotiation says whether the F understands a
cache-input job reference. The cache-channel SESSION exchange separately confirms that the
specific F-cache incarnation is reachable and agrees on a codec profile. Both conditions must
hold before C sends `CompileFileMsg` with `CACHE_REFERENCE`.

```text
scheduler assigns F
        |
        v
job connection negotiates protocol -------- no cache-input support ------> established path
        |
        v yes
C cache has/reaches SESSION_READY ---------- no, deadline expires --------> established path
        |
        v yes
allocate/bind TU key to F relationship
send CompileFileMsg(CACHE_REFERENCE)
prepare immutable TU, then send DICT/LINES on cache channel
```

The setup deadline is a configuration value and is charged to attempt time. It should be short
enough that an unavailable cache service does not hold a compile slot for a long interval. A
healthy persistent relationship normally bypasses setup entirely.

### Attempt boundary

One compile attempt uses exactly one input mode. Once a `CACHE_REFERENCE` job has been sent, C
does not send legacy chunks into that same attempt. If the cache attempt cannot reach
`INPUT_READY`, the job attempt ends and normal retry policy creates a new attempt, which may use
the established path on the same or another F. This keeps compiler-input framing and accounting
simple.

All bytes already emitted by the unsuccessful attempt remain in the byte ledger. A result table
must never report only the final attempt. Timing likewise begins at the first attempt's release
and includes retry selection and transfer.

### Supplying fallback bytes after complete-TU preparation

If `auto` selects the established path before preprocessing begins, the normal preprocessor pump
writes directly to `LegacyRemoteSink`; no PreparedTu is created. If a PreparedTu already exists
because prepare-before-assign was selected or a cache attempt ended after preparation, fallback
must not run the preprocessor again. C cache retains that exact PreparedTu until the compile
attempt is resolved and can materialize the original bytes from its immutable definitions/body
recipe through:

```text
C clone -> C cache: TAKE_LEGACY_INPUT(C_STORE_GUID, TU_SEQ)
C cache -> C clone: RAW_DATA* / RAW_END
C clone -> F clone: existing FileChunkMsg* / END
```

When preparation has already occurred, the first implementation may retain the original raw slab
until input mode/routing is resolved, avoiding a local reconstruction. The prepared-queue byte
ceiling charges both that slab and the encoded Root. After routing, a retained raw slab may be
released; a later retry reconstructs from `PreparedTU`. This is an implementation optimization
only—the bytes presented to the established path are exact in either case.

### Simulator representation

The simulator uses the same three selection modes and explicit F capability/channel state. A
fallback row includes:

- cache setup time;
- every cache-attempt C-to-F byte sent before failure;
- retry scheduling time;
- every established-path C-to-F byte;
- final compiler timing.

The present simulator has separate `raw` and `compile-only` controls but does not yet implement
this automatic decision. A result must not be labeled `auto` until the selection state machine,
attempt ledger, and fallback gates above exist.

## Product state machines

```mermaid
stateDiagram-v2
  state CClone {
    [*] --> Preprocessing
    Preprocessing --> HasReference: PREPARED(C_GUID, TU_SEQ)
    HasReference --> Assigned: scheduler selects F
    Assigned --> SelectingInput
    SelectingInput --> SendingLegacy: legacy or auto fallback
    SelectingInput --> WaitingCacheInput: SESSION_READY and cache selected
    SendingLegacy --> WaitingResult: FileChunk END
    WaitingCacheInput --> WaitingResult: cache-reference attempt running
    WaitingCacheInput --> Retry: attempt ends before INPUT_READY
    Retry --> Assigned: normal retry selection
    WaitingResult --> [*]: compile result
  }

  state CCacheTransaction {
    [*] --> Preparing
    Preparing --> Prepared
    Prepared --> Routed
    Routed --> SendingDict
    SendingDict --> SendingLines: DICT_END; Need may return
    SendingLines --> WaitingRemoteReady: LINES_END
    WaitingRemoteReady --> RemoteReady: CACHE_READY
    RemoteReady --> Retained
    Retained --> [*]: normal eviction
  }

  state FCacheTransaction {
    [*] --> ReceivingDict
    ReceivingDict --> ReceivingLinesAndDefinitions: DICT_END; send Need
    ReceivingLinesAndDefinitions --> CacheReady: LINES_END and dependencies resident
    CacheReady --> Retained
    Retained --> [*]: normal eviction
  }

  state FCloneAttachment {
    [*] --> WaitingInput
    WaitingInput --> Consuming: INPUT_READY
    Consuming --> Compiling
    Compiling --> [*]
  }
```

The C-cache transaction state is descriptive rather than stop-and-wait: after emitting one
Root, the cache immediately schedules frames for other transactions while Need/Fill proceeds.

## Executable transition trace and model correspondence

Protocol state must not be scattered across unrelated callbacks. Each state owner exposes named
transition functions, and every socket/local-session callback converts input into one of those
commands. For example:

```text
CRelationship::on_session_ready
CTransaction::on_dict_submitted
CTransaction::on_need_received
CRelationship::on_fill_applied
FTransaction::on_dict_complete
FNamespace::on_definition_installed
FTransaction::on_lines_complete
FTransaction::on_attachment_added
FTransaction::maybe_publish_ready
```

The transition function checks its allowed source state, mutates the minimum fields, and emits
one typed trace event. Codec workers return values but never execute protocol transitions. This
makes the implementation state machine reviewable and gives the formal model a concrete event
alphabet.

The optional full trace mode records:

```text
global local-event sequence
monotonic timestamp
process/store role
C_STORE_GUID and F_STORE_GUID when known
TU_SEQ, REL_SEQ, and DELIVERY_ID when applicable
event/action name
before and after transaction state
frame class, logical payload bytes, and physical socket bytes
dependency count before and after
queue and attachment identifiers
```

Normal operation keeps counters and a bounded recent-event ring. Acceptance runs enable the full
append-only event stream. An offline checker consumes that stream and verifies:

- each recorded transition exists in the protocol action table;
- DICT completes before Need for that TU;
- LINES and Fill may overlap but Ready requires both branches;
- an attachment alone never makes input ready;
- one socket writer never has two simultaneous writes;
- installed definitions never change value;
- a changed F GUID starts a new cold relationship;
- a clone detach changes attachment state but not immutable cache state;
- fallback starts a new attempt rather than changing the input mode of an active attempt;
- every C-to-F physical byte belongs to exactly one ledger frame/attempt.

The formal model should use the same action names and keys, with bounded sets of Cs, Fs, TUs,
definitions, deliveries, reconnects, and clone detachments. The large topology simulator does
not replace that state exploration: the simulator measures realistic counts and time, while the
small-state model enumerates ordering combinations. A retained implementation trace can also be
projected onto the formal action alphabet; if an emitted event or state value has no model
representation, the acceptance checker fails and the model or code must be reconciled.

## Implementation map

The production cache code should be a new library and sidecar target rather than an expansion of
`libicecc`'s existing synchronous channel implementation. Proposed file boundaries are:

| Area | Concrete responsibility |
|---|---|
| `configure.ac`, top-level `Makefile.am` | detect standalone Asio, add `cache/`, expose a build option for protocol-50 cache support |
| new `cache/Makefile.am` | build a C++23 private cache library, sidecar executable, and focused tests |
| new `cache/cache_types.*` | GUID, TU sequence, ID64, delivery ID, component descriptors, plain result/error values |
| new `cache/cache_frame.*` | cache-frame envelope, integer/column codecs, incremental frame parser, exact byte accounting |
| new `cache/cache_channel.*` | Asio relationship, reader/writer coroutines, priority queues, reconnect timers |
| new `cache/local_protocol.*` | C-clone PREPARE/ROUTE and F-clone ATTACH/TAKE_INPUT framing over Unix sockets |
| new `cache/c_store.*` | C interner authority, PreparedTU retention, route binding, Need handling, Fill construction |
| new `cache/f_store.*` | per-C-GUID namespace, DICT/LINES receive, Need calculation, Fill install, dependency wakeup, expansion |
| new `cache/namespace_runtime.*` | runtime seam for process/thread/strand alternatives |
| new `cache/service_main.cpp` | combined-role sidecar startup, local listener, descriptor handoff, lifecycle and metrics |
| `services/comm.h`, `services/comm.cpp` | protocol 50, `CACHE_SESSION` discriminator, `CompileFileMsg::InputDescriptor` serialization |
| new `client/cache_client.*` | local C-cache session and prepared-handle lifetime |
| `client/remote.cpp` | prepare/route, choose cache or legacy input, send cache reference, reconstruct fallback bytes |
| new `daemon/cache_sidecar.*` | start/watch sidecar, control socket, pass accepted cache-session descriptors |
| `daemon/main.cpp` | classify `CACHE_SESSION`; leave normal scheduling/slot accounting unchanged |
| `daemon/serve.cpp`, `daemon/workit.cpp` | branch compiler stdin source by `InputDescriptor`; attach/take input from F cache |
| scheduler | no required first implementation change; later add affinity metadata only after measurement |
| `unittests/`, `tests/` | value/frame/channel/store tests and a real multiprocess acceptance launcher |
| `capability/distribution` | one faithful event engine with physical P29/GRZ adapters and exact topology ledgers |

The precise directory names may follow maintainer preference, but the dependency direction must
remain:

```text
plain cache types and codec
        ^              ^
        |              |
cache store       cache frame codec
        ^              ^
        +------ cache I/O/Asio ------+
                                      |
                   sidecar + thin client/daemon adapters
```

`cache_types` and the codec must not include Asio. `services/comm.*` must not include store
implementation headers. This keeps protocol values reusable in tests and prevents the new I/O
library from spreading into scheduler code.

## Code-level contracts

### Prepared TU contract on C

```text
PreparedTu {
    key: (C_STORE_GUID, TU_SEQ)
    raw_length
    exact_output_digest
    dependency_manifest: immutable LOCAL32 -> ID64
    dict_components: immutable encoded buffers
    lines_components: immutable encoded buffers
    inline_material
    fallback_source: retained raw slab or reconstructable recipe
}
```

Publication is one-way: workers may build a mutable candidate, but `PreparedTu` enters the store
only after every component and exact-output field is complete. Routing holds a reference to that
object. The same PreparedTu may be routed to another F after a failed attempt without reinterning
or assigning new IDs.

### C relationship contract

One `CRelationship` owns:

```text
remote endpoint
current F_STORE_GUID or unknown-before-SESSION
SESSION state and selected codec profile
one CacheChannel
priority queues and partial write cursor
ordered PreparedTU queue with assigned REL_SEQ values
one active transaction and its unacknowledged immutable Fill batches
next committed REL_SEQ and route-state digest
reconnect state and byte counters
```

The C store is authoritative for definitions. Relationship state contains delivery knowledge,
not a second mutable definition store. Historical delivery does not suppress a later explicit
Need. Only an active unacknowledged Fill may coalesce repeated requests.

### F namespace contract

One `FNamespace` is keyed by `C_STORE_GUID` and owns:

```text
complete immutable definitions by ID64
next REL_SEQ and committed route-state digest
one active transaction overlay and missing-ID set
queued local clone attachments by TU_SEQ
materialized or streaming output state
retention/eviction metadata
current cache-channel attachment and reply queues
```

`F_STORE_GUID` identifies the whole F store incarnation, not each namespace. A reconnect from the
same C GUID reattaches the channel to the existing namespace. Different C GUIDs never address the
same definition table, even if their ID64 numeric values match.

### Transaction rendezvous contract

Root arrival and clone attachment are independent. The F transaction becomes runnable only when:

```text
dict_complete
&& lines_complete
&& unresolved_definition_count == 0
&& at least one live attachment
```

The first three conditions define `cache_ready`; attachment defines whether output should be
materialized. A cache-ready transaction may be retained without an attachment. Multiple clone
attachments are allowed only when retry policy explicitly wants them; otherwise a second active
consumer is refused as duplicate work while the cache object remains valid.

### Active dependency set

The singleton relationship does not need a cross-TU waiter index. At DICT completion, the active
transaction records its missing-ID set. Installing a complete object updates the arena store and
removes that ID from the active set. Removing the final ID posts one readiness check to the arena
owner:

```text
active transaction -> {missing ID64 set, lines_complete, attachment}
arena object store -> immutable ID64 objects retained for later REL_SEQ values
```

Different relationships perform this independently. A future wider relationship window may add
a waiter index after measurement; it is not needed in the first implementation.

## Implementation sequence and coherent commit boundaries

Each stage below produces a runnable gate. Do not begin by editing every daemon path at once.

### Stage 0: extract reusable codec/state types

Move or wrap the proven capability components behind plain interfaces without changing their
bytes. Land:

- GUID/ID64/LOCAL32/TU sequence strong types;
- immutable definition and PreparedTu representations;
- C prepare, Need-to-Fill, F install, and exact expansion interfaces;
- deterministic state digest and byte-ledger hooks;
- focused round-trip and retry tests using the existing corpus fixtures.

Acceptance: capability outputs remain byte-identical, existing capability gates pass, and the new
library can prepare/reconstruct at least one full real TU without daemon code.

### Stage 1: frame and local-protocol library

Implement cache and local envelopes as pure buffer codecs before opening sockets:

- fixed-width network-order fields;
- three-bit cache frame class and 29-bit payload length;
- component headers and chunk indices;
- incremental parser accepting every split point;
- outbound encoded-length calculation equal to actual bytes;
- local PREPARE/ROUTE/ATTACH/TAKE_INPUT messages;
- unknown extension handling defined by protocol version/profile.

Acceptance: encode/decode round trips, concatenated frames, partial headers, partial bodies,
1-MiB boundary, zero-length legal controls, rejected inconsistent chunk counts, and exact byte
counts.

### Stage 2: Asio `CacheChannel` loopback

Build the new I/O library with one in-process C/F loopback and no Icecream daemon changes:

- standalone-Asio discovery/build integration;
- one reader and one writer coroutine per relationship;
- immutable scatter/gather buffers and partial-write cursors;
- four-class priority scheduler with configurable quantum;
- per-relationship and global queue limits;
- reverse Need traffic concurrent with forward LINES;
- reconnect preserving application-level objects.

Use a raw-root integration codec initially if necessary: empty dependency DICT plus raw LINES.
It is an implementation probe, not a compression result. Acceptance proves that 20 independent
relationships can share one C uplink, each relationship has one active dialogue, Fill takes the
next permitted quantum, a slow relationship does not stop a ready relationship, and measured
socket bytes equal the frame ledger.

### Stage 3: sidecar and local clone sessions

Add `icecc-cache-service` under daemon supervision but keep remote jobs on the established path:

- sidecar startup, C/F GUID creation, Unix listener, status command, orderly shutdown;
- C clone PREPARE stream and PREPARED reply;
- route storage without remote send yet;
- F clone ATTACH/WAIT/TAKE_INPUT against an injected local transaction;
- selected NamespaceRuntime layout behind the common interface;
- byte/item memory ceilings and idle retention.

Acceptance: real preprocessor pipe -> C sidecar -> exact PreparedTu -> F sidecar handoff -> real
compiler stdin pipe. Clone disconnect at every local-message boundary must leave the sidecar able
to serve later sessions.

### Stage 4: persistent remote cache channel

Add `CACHE_SESSION` classification and descriptor handoff:

- C sidecar connects to the assigned F daemon endpoint;
- daemon consumes only the discriminator and passes the descriptor;
- F sidecar replies with GUID/profile;
- DICT/LINES/Need/Fill/ACK/READY run across the Asio channel;
- multiple TUs share one relationship;
- reconnect and replay of incomplete Root/Fill batches;
- exact relationship and per-TU ledgers.

Acceptance: two sidecars on separate loopback addresses reconstruct and compile real TUs; restart
the F sidecar to obtain a new GUID and cold Need; reconnect the same incarnation at each retained
boundary; verify no job socket carries Root or Fill.

### Stage 5: cache-reference job input and fallback

Raise the negotiated maximum to protocol 50 and extend `CompileFileMsg` only under that version:

- `LEGACY_CHUNKS` and `CACHE_REFERENCE` input descriptors;
- F job child local attachment and compiler stdin handoff;
- C `legacy`/`auto`/`cache` selection;
- short SESSION-ready deadline before selecting cache input;
- whole-attempt retry through the established path;
- accounting that retains bytes/time from all attempts.

Acceptance: old-C/new-F, new-C/old-F, new-C/new-F legacy, new-C/new-F cache, unavailable sidecar,
and interrupted cache attempt followed by established-path retry. Every case compiles the same
source through the normal compiler path, and protocol versions below 50 preserve their old wire
serialization.

### Stage 6: physical P29/GRZ profiles

Connect the research codecs only after the common transport works:

- a profile registry selects Root DICT, LINES body, and Fill encoders;
- P29 and GRZ emit the same protocol-level dependency manifest and immutable definition objects;
- profile selection is fixed for one relationship SESSION and recorded per transaction;
- codec-specific learned objects are explicit IDs, never hidden channel history;
- fallback reconstruction remains codec-independent.

Acceptance: every physical codec reconstructs and compiles all selected corpora; byte totals match
standalone codec ledgers plus measured framing; simulator physical adapters consume those same
per-TU component ledgers rather than estimated ratios.

### Stage 7: retention, rotation, and measured runtime choice

Implement the C generation and F namespace timers only after the active path is stable. Run the
same workload under process-per-GUID, thread/context-per-GUID, and shared-strand layouts where
practical. Compare:

- active and idle RSS;
- CPU time per GiB and per TU;
- cache-channel utilization and Fill latency;
- setup/teardown time for 1, 16, and 64 active C GUIDs;
- behavior when one C sends slowly or has expensive codec work;
- complete namespace reclamation after the retention deadline.

The selected runtime becomes a configuration default only after this table exists. The losing
layouts may be removed later without changing protocol or store tests.

## Build and acceptance launcher

Add one foreground launcher reachable from the normal build tree:

```text
make integration_tests
```

It should create an explicit temporary root, allocate loopback ports, start the scheduler,
iceccd instances, and cache sidecars in the foreground launcher process group, then execute
scenarios one by one. On any failure it stops launching new scenarios, asks children to exit,
waits for them, and prints the retained root. It never leaves an untracked watcher or daemon.

Minimum scenario order:

1. established-path 1C/1F reference;
2. protocol-50 raw-root 1C/1F;
3. protocol-50 physical codec 1C/1F cold then warm;
4. automatic fallback with old F;
5. automatic fallback after cache-channel attempt interruption;
6. several queued TUs on one relationship, proving one active dialogue and compile/next-transfer
   overlap;
7. 1C with 1/2/3/4/20 Fs at 200 slots/F, proving one shared C bandwidth ceiling;
8. multi-C/F namespace partition and reconnect;
9. zero-gap repeated builds;
10. retention-specific run with an explicit idle gap.

Each scenario retains:

```text
resolved configuration
process command lines and PIDs
complete stdout/stderr per process
chronological transaction/event ledger
per-TU and cumulative byte ledger
cache state/queue high-water ledger
compiler results and exact reconstructed-input digests
summary with pass/fail and elapsed/resource measurements
```

The first `make integration_tests` target now runs the deterministic scenario engine,
active-time ledger/report, simultaneous bandwidth-limit, and physical-ledger adapter tests. As
the live components land, extend that same foreground target with the ordered multiprocess
scenarios above. Unit tests remain under `make check`. Do not make long corpus performance runs
part of every incremental build. Provide a separate foreground `make protocol50_bench` launcher
that consumes named corpus manifests and retains the same ledger shape.

## Simulator correspondence and next component

The common simulator owns C count, F count, slots per F, job-selection policy, compile trace,
placement policy, and simultaneous per-route, per-C, per-F, and shared-fabric capacities.
Existing scenario files retain their original meaning: omitted per-C and per-F fields mean no
additional ceiling beyond the route and fabric. The primary
`firefox-c1f20-200b1g.json` scenario supplies every ceiling explicitly and reports the compact
label `C1F20_200B1G`.

The implemented v1-compatible fields are:

```text
network.<direction>.per_environment_bits_per_second
network.<direction>.per_worker_bits_per_second
network.<direction>.bits_per_second
network.shared_fabric_bps
```

The flow allocator applies all applicable ceilings through max-min sharing. Historical v1
scenario files and ledgers keep their original labels and hashes; no conversion or silent
reinterpretation is needed.

The current raw adapter models one complete raw-TU transfer before compilation; that is a
conservative control, not a cycle-accurate model of protocol 44's within-TU streaming. The
executable adapters are `compile-only`, `raw`, `p29`, and `grz`. The latter two require a
scenario-bound physical ledger whose producer has already passed reconstruction checks. P29 is
currently materialized only for one C and one F. GRZ is materialized as one persistent stream per
selected `(C,F)` route. Codec CPU time and automatic fallback timing are not yet scheduled, and a
report must state that boundary.

The physical-ledger adapter, active-time JSONL trace, and self-contained HTML report now share the
same event engine. That engine now executes a per-TU fork/join graph rather than forcing a phase
list to be stop-and-wait. A dependency may wait for an extent to be serialized or delivered;
input readiness and transaction commit are distinct joins; F input staging and compiler slots are
distinct resources; the two fabric directions can be capped separately; and priority traffic can
take the next bounded writer quantum. The oldest queued extent receives a turn after a configurable
maximum number of priority overtakes, so an ongoing stream of completion traffic cannot suppress
LINES indefinitely. The physical P29 projection therefore executes:

```text
Root sent ------> LINES ----------------------+--> close delivered --> input ready
Root delivered -> Need delivered -> Fill -----+
close delivered -> Ack delivered -------------------------------> state commit
attachment accepted --------------------------+------------------> input ready
```

Compilation and the final acknowledgement can overlap. A transaction and its build complete only
after compiler completion and state commit have both occurred. The current physical GRZ projection
is a one-node current-TU graph on each independent route.

The next simulator component remains one live stateful cache-channel adapter, not another
simulator. Its core objects are `PreparedTU`, `RelationshipQueue[(C,F)]`,
`ActiveTransaction[(C,F)]`, `FArena[(C,F)]`, `COutScheduler[C]`, and `CompilerPool[F]`:

1. shared C-uplink, per-F-ingress, per-route, and optional fabric limits applied simultaneously;
2. C preparation/EOF and a byte-bounded prepared queue;
3. one persistent relationship channel for every active `(C,F)` pair;
4. multiplexed DICT and LINES flows released when their TUs are prepared and routed;
5. F-generated Need at `DICT_END` using actual resident state while LINES continues;
6. C-side Fill construction from canonical definitions, with encoded-buffer reuse where useful
   but separate bytes charged to every F arena that needs the object;
7. Fill delivery that installs definitions and wakes every dependent TU;
8. job-reference arrival independent of cache-data arrival;
9. compilation start only when attachment and cache readiness both exist;
10. explicit `TU_SEQ`, per-route `REL_SEQ`, committed route-state digest, and one active dialogue
    per relationship;
11. `legacy`/`auto`/`cache` selection and whole-attempt fallback accounting;
12. exact C-to-F frame bytes, separate reverse bytes/time, CPU stages, and peak buffers.

A wider relationship window is a later measured option only if the singleton dialogue leaves the
C uplink idle. It is not part of the first comparison.

For roughly 90% timing fidelity, DICT, LINES, and Fill begin as logical bandwidth-sharing flows.
Frame overhead is charged analytically, and Fill precedence is represented at configurable
quanta. Packet-level events are unnecessary. The simulator must model both dependency branches:

```text
DICT_END -> Need -> required Fill installed --+
                                               +-> input ready
LINES_END ------------------------------------+
job attachment -------------------------------+
```

The primary repeated-build scenarios set the build gap to zero. Retention-specific scenarios may
insert a named gap. The elapsed metric remains the sum of each generation's active interval, and
`generations.tsv` remains the binding interval ledger, but metric compression is not a substitute
for zero-gap inputs because cache timers observe real simulated time.

## Required tests and measurements

### Exactness and lifecycle

- reconstruct every TU byte-for-byte and compile it through a real compiler pipe;
- job reference before Root and Root before job reference;
- global C admission order projects to contiguous `REL_SEQ` order on every F;
- one relationship keeps at most one active dialogue while different relationships progress
  concurrently;
- a committed TU's compilation overlaps the next transfer on the same relationship;
- Need emitted at `DICT_END` while the same transaction's LINES remains in flight;
- Fill inserted at the first permitted writer quantum after Need processing;
- three overlapping Need sets resulting in one transmitted copy of each active missing ID;
- Fill deliveries arriving in different orders;
- repeated Root and Fill frames producing the same retained state;
- C-clone exit, F-clone exit, and replacement attachment to the same transaction;
- cache-channel reconnect with replay of every possible unacknowledged boundary;
- new F GUID after restart producing a cold Need;
- retained Root reused by a later transaction and restored after eviction;
- frame splits at zero, one byte, selected scheduler quantum, and the 1-MiB outer limit;
- established-path fallback from an already prepared raw TU, including a failed cache attempt;
- `C1F1_1000000B1G` reference and primary `C1F20_200B1G` scenario;
- `C1F1/2/3/4/20_200B1G` with one identical shared C uplink and identical global admission order;
- independent `C16F20_200B1G` contention with explicit C/F/fabric bandwidth and both
  round-robin/bounded-sticky placement;
- zero artificial launch/build gaps in primary runs and explicit gaps only in retention runs;
- bounded memory with a deliberately large ready queue.

### Byte ledger

For every TU and cumulative boundary, retain:

```text
C->F job-reference bytes
C->F SESSION + DICT + LINES + FILL + completion-control bytes
F->C NEED + ACK/READY/control bytes
DICT and LINES bytes by component
Fill bytes by component
raw reconstructed bytes
resident and evicted cache bytes by C GUID/generation
```

Cold `Y` is every C-to-F byte for the first project build. Warm `Z` is reported for each later
build. A preinstalled package is not charged to per-build transfer. Network timing includes both
directions and the Root/Need/Fill dependency.

### Time and resource ledger

Record separately:

- preprocessor pipe time and TU EOF;
- complete raw-TU wait;
- interning and factorization;
- prepared-queue wait;
- cache-channel queue, DICT encode/send/end, LINES send/end, and frame scheduling;
- Need compute and return time;
- Fill lookup/encode/queue/send/install;
- attachment wait versus cache-data wait;
- expansion, local cache-to-clone transfer, compiler-pipe write, and compiler input EOF;
- compiler start and finish;
- per-generation active elapsed and secondary wall makespan;
- cache-channel utilization, worker utilization, RSS, and each queue high-water mark.

The current compressibility-first launch target accepts approximately 0.5 GB/s for a complete
cold path if byte targets are met. Warm throughput and simulated completion over 1-Gbit and
10-Gbit links remain separate measurements.

## Questions to settle by measurement

1. How much compile overlap is lost by complete-TU preparation compared with protocol 44?
2. Does prepare-before-assign improve remote-slot utilization enough to justify the client-order
   change?
3. Which frame quantum gives Fill low latency without excessive framing and scheduling cost?
4. Can one cache socket and I/O loop fill 1-Gbit and 10-Gbit links while workers encode/decode in
   parallel?
5. Is Unix-socket delivery from F cache within 10% of descriptor-passed/direct pipe output at 1,
   8, 16, and 32 consumers?
6. How much do affinity policies reduce cold replication across 20 and 50 Fs?
7. Can bounded expansion remove the complete reconstructed-TU buffer without changing bytes or
   throughput materially?
8. How large are retained Roots, definitions, active transactions, and materialized outputs at
   realistic concurrency?
9. For 1, 16, and 64 active C GUIDs on one F, which NamespaceRuntime layout gives the simplest
   lifecycle while staying within 10% of the best complete-path throughput?
10. Does one Asio I/O thread per active C GUID materially improve slow-client isolation or
    debuggability over a shared context with strands, and what idle RSS/thread cost does it add?
11. Is passing the accepted cache-session descriptor from `iceccd` to the sidecar measurably
    cheaper and simpler than advertising a dedicated cache port?
12. What SESSION-ready deadline gives `auto` a quick established-path fallback without discarding
    cache mode during ordinary daemon startup?
13. Across `C1F1/2/3/4/20_200B1G`, what is the smallest warm F frontier that prevents compiler
    starvation after accounting for the shared C uplink and repeated Fill?
14. Which prepared-queue TU/byte ceilings retain producer parallelism without holding multiple
    copies of too many large preprocessed inputs?

These measurements share one scenario launcher and one codec adapter. The interaction protocol
does not change with P29, GRZ, a learned block predictor, or the chosen component entropy coder.

## Requested bigoracle review

Please review the strategy as a product implementation, while treating the distribution
simulator as its measurement companion. The highest-value review questions are:

1. Is the separation between the per-job connection and persistent cache channel the smallest
   coherent change to existing Icecream?
2. Can the same-daemon-port `CACHE_SESSION` handoff be simplified further without adding
   scheduler state or one bulk connection per job?
3. Is `CompileFileMsg::InputDescriptor` the correct and sufficient job/cache rendezvous seam?
4. Does DICT contain everything F needs to emit its one exact Need at `DICT_END`, independently
   of LINES arrival, while naming the expected predecessor route-state digest?
5. Is one active missing-ID set per relationship sufficient for the first implementation, with
   object reuse supplied by the arena store and concurrency supplied across F relationships?
6. Which F NamespaceRuntime should be the first implementation: process per C GUID, dedicated
   thread/Asio context per C GUID, or shared contexts with a strand per GUID? In particular,
   please evaluate the dedicated-thread option rather than assuming it must eventually be the
   shared-strand design.
7. Does the fallback attempt boundary preserve a simple established path while avoiding repeated
   preprocessing?
8. Are any Stage 0–5 boundaries ceremony that can be collapsed without losing an independently
   runnable gate?
9. Which retained objects can be removed from the first version while still permitting retry,
   reconnect, and cache reuse?
10. Are the primary topology semantics and zero-gap launch rules sufficient to prevent a
    per-route-bandwidth result from being mistaken for a shared-C-uplink result?
11. Does the `C1F1/2/3/4/20` width sweep expose enough information to choose a bounded sticky
    frontier before adding independent multi-C scenarios?

Prefer simplifications that remove objects, states, messages, copies, or commit stages. Preserve
the essential snap-together boundaries:

```text
codec/state library
    <-> cache store
    <-> cache channel
    <-> thin C/F clone adapters
```

The first accepted implementation does not need every later optimization. It does need one exact
full-TU round trip, real compiler-pipe execution, automatic fallback, concurrent relationships,
one ordered dialogue per relationship, bounded memory, and a byte/time ledger that agrees with
the physical socket path.
