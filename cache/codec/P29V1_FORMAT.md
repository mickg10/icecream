# P29v1 format

P29V1 is profile ID 1 in [CacheWire revision 1](../P50_PROTOCOL.md).
Its implementation is [p29_wire.h](p29_wire.h), using the Line/Region interner
in [p29_intern.h](p29_intern.h) and the transactional occurrence matcher in
[p29_online_s1.h](p29_online_s1.h). This specifies the current inner format,
not the historical P29 v0 fixtures in [FORMAT.md](FORMAT.md).

## Representation

Input is reconstructed exactly from ordered Regions. The interner identifies
equal Lines and Regions; OnlineS1 replaces repeated Region sequences with
Block references or definitions. Default matcher parameters are
`min_match=3, max_chain=1024, hash_bits=22`. Encoder matching choices affect
compression, not the decoder grammar.

C sends the Root and any new Block definitions. F requests missing Regions
in their exact first-use closure order. C fills only that request, using
literal bytes, reusable Lines, line markers and, when negotiated, local
system-source Lines. The decoder stages changes until the outer transaction
has validated the complete raw length and digest. No Key64 appears in the
inner wire.

## CacheWire carriage

The BODY descriptor encoding is 1. Its `encoded_bytes` and digest describe
the complete inner BODY stream; its `decoded_bytes` is the number of Root
references, **not a raw byte count**. TX_BEGIN separately carries the exact
raw byte count and digest.

BODY concatenates inner frames directly. NEED and FILL each start with one
u64 big-endian **total inner-stream byte length** in their first outer
payload; continuation payloads contain only inner bytes. The prefix is not
repeated and there is no flags word. Even with no missing Regions or new
Blocks, there is an outer NEED exchange containing only the empty inner
TU_END (and the eight-byte outer stream-length prefix). The inner NEED frame
itself is absent in that case. Declared lengths, bounded continuations and
frame order must close exactly.

The codec API returns an optional NEED frame from `receive_body()`; the
product wrapper appends the empty TU_END before sending it. The codec's
post-FILL close frame is useful in codec fixtures but is not an extra network
response: the endpoint sends outer TX_COMMIT after input publication.

## Inner frames and entropy coding

An inner header is kind u8 plus payload length u32 **little-endian**, unlike
the outer CacheWire header. Current kinds are:

| Value | Kind |
|---:|---|
| 1 | ROOT |
| 2 | BLOCKDEF |
| 3 | NEED |
| 5 | PATHDEF |
| 8 | FILL_CTRL |
| 9 | FILL_LIT |
| 254 | TU_END |

Other research frame kinds are not accepted by this core. Unless explicitly
fixed-width, counts, IDs, lengths and operands in decoded payloads use minimal
unsigned LEB128; signed Region deltas use zigzag encoding. The opcode byte
and marker flag bytes are individual bytes.
A digest is its canonical 16 stored bytes.

ROOT and TU_END are unconditional.  BLOCKDEF, NEED, FILL_CTRL, FILL_LIT, and
PATHDEF are present only when nonempty.  A committed TU therefore has these
ordered streams (brackets mean conditional):

- C BODY: `ROOT [BLOCKDEF]`
- F NEED: `[NEED] TU_END` (the product wrapper appends the empty terminator)
- C FILL: `[FILL_CTRL] [FILL_LIT] [PATHDEF] TU_END`
- F close: outer `TX_COMMIT` (codec-only fixtures also record an empty `TU_END`)

ROOT, BLOCKDEF, NEED, and PATHDEF payloads are independent zstd-3 messages.
FILL_CTRL and FILL_LIT are two separate continuing zstd-3 streams with the
content-size field disabled.  Each nonempty TU ends in `ZSTD_e_flush`; an
explicit final close adds `ZSTD_e_end`.  FILL's TU_END has one payload byte:
bit 0 records a FILL_CTRL segment and bit 1 a FILL_LIT segment. The codec's
closing TU_END has an empty payload; the live endpoint returns outer TX_COMMIT
instead. The mask describes segment presence, not whether entropy streams
were finally closed: the codec caller supplies `close_entropy` separately.
FILL opcode 3 and every frame kind outside this core are forbidden (frame
kind 3 is the valid NEED frame).

### ROOT and BLOCKDEF raw payloads

ROOT is a sequence of minimal unsigned LEB128 typed tags.  Region `r` is
`2*r`; Block `b` is `2*b+1`.

BLOCKDEF begins with definition count.  Each definition is Block id, kind,
then one of:

- kind 0: child count followed by that many Region ids;
- kind 1: source occurrence offset and Region count, copying a preceding
  contiguous range of F's committed occurrence stream.

Definitions occur in first-use order.  A Block already known by F may not be
redefined.  Every Root Region and every Block child must be known already or
defined by the matching FILL before TU_END.

### NEED raw payload

NEED is missing-Region count, those Region ids in first-use closure order,
then a zero terminator.  BLOCKDEF presence makes NEED present even when the
missing-Region count is zero.  C validates the complete list and rejects any
unsolicited, reordered, extra, or absent request.

### FILL_CTRL and FILL_LIT raw payloads

FILL_CTRL starts with the requested-Region count.  For each Region, it stores
the exact raw byte length followed by operations until exactly that length is
materialized.  FILL_LIT is the shared literal-byte stream consumed by
operations 0 and 6.  The allowed operation program is:

| Opcode | Operands | Meaning |
| ---: | --- | --- |
| 0 | literal length | Copy that many bytes from FILL_LIT. |
| 1 | source-Region zigzag delta, offset, length | Copy a verified Line view and publish it as the next public Line. |
| 2 | public-Line id | Copy a previously published Line view. |
| 4 | Path id, line number, flag count, flag bytes | Reconstruct a preprocessor line marker. |
| 5 | Path id, source-Line id | Copy that exact Line from an allowed system source. |
| 6 | Path id, source-Line id, prefix, suffix, middle length | Copy source prefix and suffix around middle bytes from FILL_LIT. |

Opcode 1's delta is `source_region-current_region`.  Public-Line zero is a
sentinel and never appears on the wire.  Opcode 5 or 6 is legal only for an
exact Path under `/usr/include/`, `/usr/lib/gcc/`, or `/usr/local/include/`,
and only when system-source reuse was enabled by equal nonzero C/F
fingerprints in SESSION_HELLO/SESSION_STATE. Path eligibility alone does not
enable reuse.
The provider supplies immutable source bytes and complete line offsets;
missing, malformed, or out-of-range source views fail the TU.  Valid but
different host source bytes are detected by the outer transaction's raw
digest/materialization check and the TU is abandoned.

PATHDEF is a sequence of path-byte-length followed by exact path bytes.  Its
frame is physically after the continuing streams, but F stages and validates
it before interpreting marker/source operations.

### State and transaction law

All persistent route state belongs to the provider.  C owns Path ids,
per-Line mixed state, the next public-Line id, and its mirrors of F-known
Regions and Blocks.  F owns Paths, immutable per-committed-TU Region byte
segments and Region views, public-Line views, Blocks, and the occurrence
stream.  The serializer/deserializer allow exactly one pending TU.

BODY, NEED, decoded definitions, and materialization remain staged.  Commit
first verifies the captured provider-state base, pre-reserves every append,
publishes immutable Regions/Blocks through the provider, then applies the
redo. Abandon discards the redo and resets the continuing zstd contexts, so
a resend starts fresh segments. This local operation does not authorize a
route reset or replacement of uncertain work; the endpoint owns reconciliation.

`P29WireLimits` bounds Region/Block/Path/public-Line ordinals, Region and TU
bytes, Block children, occurrence count, decompressed messages, and continuing
stream output.  Frame order, minimal closure, requested identities, lengths,
source bounds, and trailing bytes are all checked before provider state moves.

The template defaults are 2^24 Regions, Blocks and public Lines; 2^20 Paths;
2 GiB per Region; 1 GiB per TU; 2^27 Block children; and 2^30 occurrences.
Product providers set their byte bounds from endpoint limits; these defaults
are not a promise that all those capacities can be allocated simultaneously.
NEED's inner-byte bound is min(max_tu_bytes, 20 + 5*max_occurrences);
FILL's is 2*max_tu_bytes + 1 MiB, using overflow-safe calculations.

## Verification

The retained `v1core-*.bin` codec witnesses contain 120 C-to-F and 40 F-to-C
frames for 20 TUs. Additional warm, edited-turn and corpus witnesses exercise
reuse; their provenance is recorded in the frozen golden manifest. Compressed
zstd bytes may differ between library versions: cross-version checks compare
frame order, kind and exact decoded payloads. Same-build repeatability and
cross-provider checks still require byte equality. Golden vectors are not
compression-effectiveness measurements and must not be regenerated to hide
a format regression.
