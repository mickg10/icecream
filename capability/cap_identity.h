#pragma once
#include <cstdint>
#include <array>
// -----------------------------------------------------------------------------
// icecream #16 capability harness — product v1 identity (BigOracle P25 ruling).
//
// One opaque 128-bit SourceGeneration is latched ONCE at the start of a C<->F
// relationship.  Every object is then addressed only by a generation-local typed
// ordinal.  There is NO per-object u64 key (that was P25's redundant association
// plane, ~5.46 MB / 16 corpora; collapsing it returns to ~P24 wire).  The ordinal
// is the sole identity used identically for:
//     wire reference · C immutable-store lookup · F cache lookup · NEED response
// Ordinals are dense and monotonic and are never reused within a generation.
// A generation changes only on restart or explicit compaction; old generations are
// retained until their Roots and leases retire.  No remapping protocol exists until
// a real compaction / sparse-ID measurement proves it pays.
// -----------------------------------------------------------------------------
namespace cap {

using SourceGeneration = std::array<uint8_t, 16>;   // opaque 128-bit epoch, latched out-of-band

enum class ObjectKind : uint8_t { Region = 0, Block = 1, PublicLine = 2, Root = 3 };

// The only identity transmitted in frames: (kind, generation-local u32 ordinal).
// SourceGeneration is latched separately and never repeated per object.
struct ObjectKey {
    ObjectKind kind;
    uint32_t   ordinal;   // generation-local, monotonic, never reused within a generation
    bool operator==(const ObjectKey& o) const { return kind == o.kind && ordinal == o.ordinal; }
};

// Per-generation, per-kind monotonic ordinal allocator.  Dense; resets only on a new
// generation.  C allocates on first publication; F learns the same ordinals from the
// decoded ROOT/manifest and NEED reply, so both sides agree without a separate key map.
struct OrdinalAllocator {
    uint32_t next_region = 0, next_block = 0, next_public_line = 0, next_root = 0;
    uint32_t alloc(ObjectKind k) {
        switch (k) {
            case ObjectKind::Region:     return next_region++;
            case ObjectKind::Block:      return next_block++;
            case ObjectKind::PublicLine: return next_public_line++;
            case ObjectKind::Root:       return next_root++;
        }
        return 0;   // unreachable
    }
    uint32_t count(ObjectKind k) const {
        switch (k) {
            case ObjectKind::Region:     return next_region;
            case ObjectKind::Block:      return next_block;
            case ObjectKind::PublicLine: return next_public_line;
            case ObjectKind::Root:       return next_root;
        }
        return 0;
    }
};

}  // namespace cap
