# R6 legacy input attachment seams

This branch is a behavior-preserving preparation step for the first product-integrated Protocol
50 compile. It does **not** enable a cache mode, expose a cache service through the daemon, or
claim M3/R6 completion.

The client selects only `LegacyRemoteSink`, which retains the existing preprocessed-file-descriptor
to `FileChunkMsg`/`EndMsg` path. The sink owns the descriptor for the complete call, including
read failures, ordinary send failures, and a terminal `STATUS_TEXT` received while a send is
failing. A terminal response cannot bypass descriptor closure or message destruction.

The daemon selects only `LegacyChunkSource` inside the existing `work_it` poll/compiler/result
loop. Compiler creation, stdin ownership, output collection, result generation, termination, and
connection lifetime stay in `work_it`. The source object is constructed on the parent side after
the compiler fork and introduces no new connection. A nonempty pending chunk must either advance
its compiler-stdin offset or fail; a zero-byte write is not permitted to create a writable-poll
busy loop.

No cache connection exists in this branch to transfer or inherit across the compiler fork.

## Environment archive boundary

The compiler-environment archive is not preprocessed source and must never pass through a future
cache/source-mode selector. The current legacy-only branch uses identical `FileChunkMsg` mechanics
for both streams, so behavior is unchanged today. Before a `CacheAttachmentSource` or selectable
`RemoteInputSink` is enabled, `build_remote_int` must keep environment installation on an explicit
legacy transfer path and construct/select the source sink only after environment transfer and
verification have completed. This is an implementation gate, not another wire protocol.

`CacheAttachmentSource` remains out of scope until the reviewed Protocol 50 product prerequisites
are accepted: exact-byte idempotent prepare replay, revision-map activation ordering and aggregate
owner caps, and the single-owner `InputRecord`/late-loser semantics. This branch adds none of those
records, wire fields, advertisement, selection, fallback, networking, or result behavior.
