// line_event.h — the AUTHORITATIVE chronological event + serializer/decoder contract for the
// issue #16 cross-TU compression bake-off. Both contenders (codec50 structure codec + the
// definition-plane codec / semantic-Root codec) MUST consume this identical event stream and
// honor the identical decoder semantics, so their FinalRatio / throughput numbers are
// apples-to-apples over one cold chronological pass.
//
// Ownership: implementer (superblock). defcodec owns definition_codec.{h,cpp}; this header is the
// shared substrate ABOVE both — it does not #include either codec. Header-only, no .cpp.
//
// ---------------------------------------------------------------------------------------------
// THE MODEL (exactly what the trace interner emits; see Interner::process in codec50.cpp)
// ---------------------------------------------------------------------------------------------
// Input = a chronological sequence of TUs (preprocessed .ii blobs) in submit order. The interner
// walks each TU's bytes and emits, in exact source order:
//   * a stream of REGION occurrences  — a region is a marker-delimited span ('# ' #line boundary);
//   * each region expands to an ordered stream of LINE occurrences;
//   * each distinct line has stable, content-addressed, immutable keys (same bytes => same key,
//     forever, across all TUs and builds). A line is either a preprocessor MARKER
//     (# lineno "path" flags) or a LITERAL (arbitrary bytes incl. its '\n' terminator).
//
// The flattened LINE-level view is the canonical event stream both codecs consume:

#pragma once
#include <cstdint>
#include <cstddef>

namespace iceline {

using TuId     = uint32_t;   // 0-based, chronological (submit order)
using RegionKey = uint32_t;  // stable id of a marker-delimited region (immutable within a generation)
using LineKey   = uint32_t;  // stable content-addressed id of a distinct line (1-based; immutable)

// One chronological line occurrence. `bytes`/`len` are only guaranteed valid during the callback
// (they point into the interner arena); a codec that keeps them past first_seen must copy.
struct LineEvent {
    TuId       tu;            // which TU this occurrence belongs to
    RegionKey  region;       // the region (marker-delimited span) this line belongs to
    LineKey    line;         // stable id of this line's exact bytes
    const uint8_t* bytes;    // the line's raw bytes, INCLUDING its terminator; concatenating these
    uint32_t   len;          //   in event order reproduces the TU's exact preprocessor output
    bool       region_start; // true iff this line is the first line of `region` in THIS TU occurrence
    bool       first_seen;   // true the FIRST time `line` (this key) appears anywhere, chronologically
    bool       tu_start;     // true iff this is the first event of TU `tu`
    // --- source-location context (for HPDC TOOLCHAIN_BASE matching + template/config-variant prediction) ---
    // Populated from the governing preprocessor marker ('# lineno "path" flags'): a MARKER line carries its
    // own parsed values; a LITERAL line inherits the current path_id and a logical_line that advances by 1
    // per emitted source line since the last marker. Both codecs derive these identically, so a predictor may
    // key a line's base candidate on (path_id, logical_line) — the toolchain/system headers that dominate every
    // .ii live at stable source locations, so the same line reappears at the same (path_id, logical_line).
    uint32_t   path_id;      // interned #line path of this line's source location (stable across TUs/builds)
    uint32_t   logical_line; // source line number within path_id (marker lineno, +1 per literal line after it)
    uint32_t   marker_flags; // packed marker flags for a MARKER line (bit0 push /bit1 return /bit2 system /bit3 extern-C); 0 for a literal
};

// The event SOURCE. A codec drives the pass by pulling events in strict chronological order.
// Semantics both codecs rely on:
//   * Events arrive in exact preprocessor order; TU boundaries via tu_start.
//   * first_seen fires exactly once per LineKey (the ONLY time its bytes must be shipped/defined).
//   * region_start marks region boundaries so a codec may factor at the region level; a region's
//     line-id sequence is fixed by its RegionKey (region composition is immutable).
//   * "Semantic Root" of a TU := the ordered vector of its RegionKeys (region_start events), i.e.
//     the TU's meaning independent of any Block/Slice covering. S0 memoization keys on this.
struct EventSource {
    virtual ~EventSource() = default;
    // Invoke `sink(ev)` for every LineEvent of the whole corpus in chronological order.
    // Return the total raw byte count (sum of TU lengths) for ratio denominators.
    virtual uint64_t drive(void (*sink)(const LineEvent&, void* ctx), void* ctx) = 0;
    // Random access a region's canonical line-id composition (for region-level factoring / decode).
    virtual const LineKey* region_lines(RegionKey r, uint32_t& count) const = 0;
};

// ---------------------------------------------------------------------------------------------
// THE DECODER CONTRACT (F side) — identical for both contenders, verified by whole-source digest.
// ---------------------------------------------------------------------------------------------
// F holds an INDEPENDENT store built ONLY from received wire bytes (never the interner). It must:
//   1. On a shipped line definition (a first_seen line): install LineKey -> exact bytes.
//   2. On a shipped region definition: install RegionKey -> its LineKey sequence.
//   3. Reconstruct a TU by expanding its Root (RegionKey sequence) -> per region, concatenate its
//      lines' bytes in order. Result MUST byte-exactly equal the original TU.
// A codec is CORRECT iff, for every TU, F's reconstruction == the interner's original bytes AND a
// whole-source digest matches. No hash is trusted for correctness (exact compare on cache insert).
//
// WIRE ACCOUNTING (holistic FinalRatio): every byte a codec puts on the wire in one cold pass is
// counted — definitions (line/region/path/block), the per-TU root/occurrence stream, MISSING/FILL
// round-trips, and framing. Compression is zstd level <= 3 for the STRUCTURE legs; the definition
// plane may use its own entropy backend (owner ruling: fast entropy coder on the one-time dict).
// FinalRatio = sum(raw TU bytes) / sum(all wire bytes).
//
// S0 ROOT_REF (semantic-Root memoization, amortized across builds): once a TU has published its
// Root (root_key := hash of its RegionKey sequence), any LATER TU (strict online: published before
// t) whose Root matches EXACTLY ships only ROOT_REF{ txid:u32, raw_len:u64, root_key:u64 } (32 B
// C->F) + ACK{ txid:u32, digest:u128 } (24 B F->C) — no DICT/FILL, since the defining Root already
// installed the closure in the sticky-F conversation. Root identity is the semantic RegionKey
// sequence, so later Block/Slice learning can never change a published Root's meaning.

// ---------------------------------------------------------------------------------------------
// SOURCE ORACLE — the original source/header files as SIDE INFORMATION (both C and F have it: the
// icecream environment ships the toolchain + system headers, and the submitting client has the
// project tree). A preprocessed Region is mostly VERBATIM source lines, so a Region can be
// materialized as a compact COPY-PROGRAM against its originating source file — reference
// source_lines(path_id, first, count) instead of shipping the novel line text — turning the
// "line-dictionary entropy wall" into near-zero structure. path_id / logical_line on each LineEvent
// give the coordinate; this oracle returns the bytes at that coordinate. BOTH sides must return
// byte-identical results for the same (path_id, first, count) — the codec still verifies each
// materialized Region against the interner's exact bytes and falls back to shipping text on any
// mismatch (a header the worker's copy differs from), so correctness never depends on the oracle.
struct SourceOracle {
    virtual ~SourceOracle() = default;
    // Raw bytes of source lines [first, first+count) of file path_id (1-based logical line `first`,
    // each line INCLUDING its terminator). Sets out_len; returns the pointer, or nullptr if the file
    // or range is unavailable (then the codec ships the line text — no correctness dependence).
    virtual const uint8_t* source_lines(uint32_t path_id, uint32_t first, uint32_t count, uint32_t& out_len) const = 0;
    // Whole-file bytes of path_id (for building a longest-match COPY index over the source). nullptr if absent.
    virtual const uint8_t* source_file(uint32_t path_id, uint32_t& out_len) const = 0;
    // Map an interned #line path string to a stable path_id (the same id carried on LineEvent.path_id).
    virtual uint32_t path_id_of(const char* path, uint32_t len) const = 0;
};

} // namespace iceline
