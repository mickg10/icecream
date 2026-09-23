# Historical codec formats

This file freezes the formats that existed at product commit
`db9870d903be76694b01158303cf9e438f802120` and the named research formats at
`56fff4e40f7ba67befa983772a29e2813233fcf2`. The v0 sections below describe
historical fixtures, not the current CacheWire revision-1 transport. Paths and
line numbers in those sections refer to the named revisions, including files
since removed or relocated. Use [the current CacheWire overview](../README.md)
for the production outer wire and supported profile IDs.

Current profiles are specified in [P29V1_FORMAT.md](P29V1_FORMAT.md) and
[ZSTD_FORMATS.md](ZSTD_FORMATS.md). This file is only the retained format
catalogue needed to interpret frozen fixtures; it is not a current protocol
or performance specification.

Every integer below is unsigned.  `u16be`, `u32be`, and `u64be` are fixed-width
big-endian integers; `u32le` and `u64le` are fixed-width little-endian
integers.  A digest is its 16 stored bytes without another length prefix.
Offsets begin at zero.  The committed files and hashes in
`unittests/codec_golden/MANIFEST.json` are the executable witnesses.

## Historical v0 Protocol-50 carriage

The outer frame header is four bytes: message type in byte 0 and payload
length as a 24-bit big-endian integer in bytes 1..3, followed by exactly that
many payload bytes.  The initial payload cap is 1 MiB.  Source:
`cache/protocol50.cpp:586` (`encode_frame_header`).

The message types used here are `TX_BEGIN=5`, `DICT=6`, `BODY=7`, `NEED=8`,
`FILL=9`, and `TX_COMMIT=10`; the enum authority is
`cache/protocol50.h:103`.  DICT, BODY, NEED, and FILL message payloads are
opaque bytes at this layer (`cache/protocol50.cpp:446`).

### ComponentDescriptor (34 bytes)

`cache/protocol50.h:122` and `encode_descriptor` in
`cache/protocol50.cpp:243` define:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 2 | encoding, u16be |
| 2 | 8 | encoded byte count, u64be |
| 10 | 8 | decoded byte count, u64be |
| 18 | 16 | digest of encoded bytes |

### TxBegin (152 bytes)

`cache/protocol50.h:227` and the `TxBegin` arm of
`cache/protocol50.cpp:446` define:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | history nonce, u64be |
| 8 | 8 | route-relative sequence, u64be |
| 16 | 8 | TU sequence, u64be |
| 24 | 2 | profile id, u16be |
| 26 | 2 | P29 root mode, u16be |
| 28 | 16 | pre-state digest |
| 44 | 34 | DICT ComponentDescriptor |
| 78 | 34 | BODY ComponentDescriptor |
| 112 | 8 | exact raw byte count, u64be |
| 120 | 16 | exact raw digest |
| 136 | 16 | transaction digest |

The 152-byte size is also frozen as `kMandatoryControlFramePayload` in
`cache/protocol50.h:107`.

### NEED byte stream

`encode_need_messages` at `cache/protocol50.cpp:659` emits a stream that can
be split across NEED messages.  The first chunk starts with key count u64be
and encoded-delta byte count u64be.  The remainder is a sequence of unsigned
minimal LEB128 deltas over strictly increasing `Key64` wire values.  Later
chunks contain only continuation bytes from that delta sequence.

### FILL byte stream and FillRecord

`FillMessage` is an opaque byte vector (`cache/protocol50.h:268`).  Concatenated
FILL payloads form records produced at `cache/protocol50.cpp:735`:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | bytes after this field, u64be; at least 32 |
| 8 | 8 | object `Key64`, u64be |
| 16 | 16 | canonical object-content digest |
| 32 | 8 | canonical object byte count, u64be |
| 40 | variable | canonical object bytes |

The record model is `FillRecord` at `cache/protocol50.h:336`.  Message
boundaries need not coincide with record boundaries.

## P29 product v0

The DICT descriptor encoding is `kP29KeyVectorEncoding=1`; BODY uses either
that legacy key vector or `kP29ResidualBodyEncoding=2`.  These values are
defined at `cache/p50_slice0.h:35,39`.

### Key vector (encoding 1)

`encode_key_vector` at `cache/p50_slice0.cpp:395` emits zero bytes for an
empty vector.  Otherwise it emits a u32be key count followed by that many
u64be `Key64` values.  The v0 DICT is the complete transitive manifest in this
form.  The root embedded in a residual BODY is also this form.

### Canonical object payload

`encode_payload(ObjectType, ObjectPayload)` at `cache/p50_slice0.cpp:293`
emits:

- Byte object: kind byte `0`, payload byte count u64be, then exact bytes.
- Child object: kind byte `1`, child count u64be, then that many u64be
  `Key64` values.

These are the object bytes carried inside each FillRecord.

### Residual BODY (encoding 2)

`encode_p29_residual_body` at `cache/p50_slice0.cpp:156` emits exactly:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `P29R` (`kP29ResidualMagic`, line 17) |
| 4 | 8 | root-vector bytes, u64le |
| 12 | 8 | residual-line count, u64le |
| 20 | 8 | alpha control bytes, u64le |
| 28 | 8 | alpha data bytes, u64le |
| 36 | 8 | residual frame bytes, u64le |
| 44 | variable | root key vector |
| next | 16 per line | line `Key64` u64le, then raw line size u64le |
| next | variable | residual-group frame |

The decompressed residual-group payload is empty when the line count is zero.
Otherwise it is alpha control size u64le, alpha data size u64le, then control
bytes and data bytes.  The two sizes must agree with the copies in the outer
BODY header.

The residual-group frame is one u32le selector/length word followed by its
payload.  Bits 31..29 select `0=zstd-3`, `1=libbsc BWT+QLFC`, or `2=zstd-10`;
bits 28..0 are the exact payload length.  This is defined in
`research/codecs/residual_group_codec.h:1-14`.

## ZSTD product v0

Both ZSTD_TU and ZSTD_ROUTE use an empty DICT with encoding `0` and a BODY
with encoding `1` (`cache/p50_zstd.h:13-20`).  BODY is one independently
terminated zstd frame; there is no additional codec-local header.

`zstd_route_product_v0` sets compression level 3 and window log 27.  It
references, but does not transmit separately, the exact committed raw suffix
whose length is `min(max_history_bytes, 2^window_log)`; product defaults make
that at most 128 MiB.  Source: `set_route_compression_parameters` and
`route_history_limit` at `cache/p50_zstd.cpp:262,273`.  Parameters not set by
those functions remain library defaults, recorded as `-1` in the tuple.

ZSTD_TU has the same BODY encoding but no route prefix.  Its committed golden
is retained separately under `product/tu/`; it is not a claim that TU and
ROUTE tuple semantics are interchangeable.

## GRZ product v0

The retained v0 group container was emitted by `group_pack` at
`cache/p50_grz.cpp:216`.  All integers in this container are little-endian:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `0x31505247` (`GRP1`) |
| 4 | 4 | number of 64-KiB groups, u32le |
| 8 | 8 | COPY-reference count, u64le |
| 16 | 8 | token count, u64le |
| 24 | 8 | committed history bytes, u64le |
| 32 | 13 per token | kind u8, value u64le, length u32le |
| next | 8 | residual-group frame size, u64le |
| next | variable | residual-group frame described above |

Token kind `1` is COPY; kind `0` is ADD.  A COPY value is an absolute offset
in committed-history-plus-current-output.  An ADD value is its offset in the
decoded residual stream.  Every token has nonzero length, and all declared
counts, bounds, and trailing bytes are checked by `group_unpack`.

The frozen product values are 64 KiB per group, 16-byte anchors, at most
16 Ki anchor entries, and a retained suffix of at most 128 MiB.  Anchors are
planted at every residual byte until the cap; rebuild retains the newest
16 Ki possible positions.  Source: `cache/p50_grz.cpp:21-27,94-187`.

## Research ROUTE formats

`zstd_route_research_thin` and `zstd_route_research_ldm31` are zstd frames
with route history supplied as a prefix, just like the product ROUTE body,
but every parameter is explicit.  Thin uses level 3, dFast strategy 2,
windowLog 27, LDM hash log 20, minimum match 64, bucket log 3, and hash-rate
log 7.  LDM31 changes windowLog to 31 and LDM hash log to 24; both use hashLog
17, chainLog 16, searchLog 1, minMatch 5, targetLength 0, disable content-size,
checksum, and dictionary-id fields, and use one compression thread.  Authority:
`capability/distribution/ZSTD-SIMULATOR-AUDIT.md:28-45` at `56fff4e4`.

## Research GRZ2/G2 container

All values are little-endian.  At `56fff4e4`, `grz2g.cpp:115-135` defines
the magics `GRZ3=0x335a5247`, `GRPF=0x46505247`, and `GEND=0x444e4547`.

The stream header is: magic u32le, version u32le (`3`), mode u8, anchor bytes
u32le, spacing bits u32le, table bits u32le, literal backend u8, token backend
u8, selection flag u8, literal-block raw cap u64le, then group-close TU count,
raw cap, ADD cap, retained history, and anchor budget as five u64le values
(`grz2g.cpp:282-291`).

Each group is: `GRPF` u32le; group index, absolute start, raw output bytes,
history base, and history extent as five u64le values; TU count u32le followed
by that many TU lengths u64le; offset mode u8; four backend bytes (literal
lengths, first literal block or STORE, source deltas, match lengths); four raw
stream sizes u64le; four encoded stream sizes u64le; literal-block count
u32le; for each block encoded size u64le and backend u8; group digest u64le;
then the four encoded streams in the declared order.  Source:
`grz2g.cpp:491-516` at `56fff4e4`.

The final frame is `GEND` u32le followed by total raw bytes, group count,
match count, and stream digest as four u64le values (`grz2g.cpp:541-545`).
