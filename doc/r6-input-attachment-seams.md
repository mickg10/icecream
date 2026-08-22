# R6 legacy input attachment seams

This branch is a behavior-preserving preparation step for the first product-integrated Protocol
50 compile. It does **not** enable a cache mode, expose a cache service through the daemon, or
claim M3/R6 completion.

The client selects only `LegacyRemoteSink`, which retains the existing preprocessed-file-descriptor
to `FileChunkMsg`/`EndMsg` path. The daemon selects only `LegacyChunkSource` inside the existing
`work_it` poll/compiler/result loop. Compiler creation, stdin ownership, output collection, result
generation, termination, and connection lifetime stay in `work_it`. The source object is
constructed on the parent side after the compiler fork and introduces no new connection. No cache
connection exists in this branch to transfer or inherit across that fork.

`CacheAttachmentSource` remains out of scope until the reviewed Protocol 50 product prerequisites
are accepted: exact-byte idempotent prepare replay, revision-map activation ordering and aggregate
owner caps, and the single-owner `InputRecord`/late-loser semantics. This branch adds none of those
records, wire fields, advertisement, selection, fallback, networking, or result behavior.
