#pragma once
// icecream #16 capability harness — cap_protocol.h
// (De)serialization of the per-TU C<->F dialogue payloads carried inside cap_transport
// frames.  Sub-framing only; the codec semantics live in cap_codec.  Every blob is
// length-prefixed with a varint so the reader needs no side-channel counts.
//
//   Root (C->F) = blob(rootb) ++ blob(blockRaw)          blockRaw empty => no new blocks
//   Need (F->C) = missingRaw                              (varint cnt ++ ids ++ varint 0)
//   Fill (C->F) = blob(fill_paths) ++ blob(m0)..blob(m3) mixed parts, empty when absent
#include <array>
#include <cstdint>
#include <vector>
#include "cap_codec.h"

namespace capp {

static inline void put_blob(std::vector<uint8_t>& o, const std::vector<uint8_t>& b){
    capc::put_varint(o, b.size());
    o.insert(o.end(), b.begin(), b.end());
}
static inline std::vector<uint8_t> get_blob(const uint8_t*& p, const uint8_t* e){
    uint64_t n = capc::get_varint(p);
    if(uint64_t(e-p) < n){ fprintf(stderr,"protocol: truncated blob\n"); exit(2); }
    std::vector<uint8_t> b(p, p+n); p += n; return b;
}

// ---- Root ----
static inline std::vector<uint8_t> pack_root(const std::vector<uint8_t>& rootb,
                                             const std::vector<uint8_t>& blockRaw){
    std::vector<uint8_t> o; put_blob(o, rootb); put_blob(o, blockRaw); return o;
}
static inline void unpack_root(const std::vector<uint8_t>& payload,
                               std::vector<uint8_t>& rootb, std::vector<uint8_t>& blockRaw){
    const uint8_t* p = payload.data(); const uint8_t* e = p + payload.size();
    rootb = get_blob(p, e); blockRaw = get_blob(p, e);
    if(p != e){ fprintf(stderr,"protocol: root trailing bytes\n"); exit(2); }
}

// ---- Fill ----
static inline std::vector<uint8_t> pack_fill(const std::vector<uint8_t>& fill_paths,
                                             const std::array<std::vector<uint8_t>,6>& mixedEncoded,
                                             size_t mixedPartCount){
    std::vector<uint8_t> o; put_blob(o, fill_paths);
    for(size_t i=0;i<mixedPartCount;++i) put_blob(o, mixedEncoded[i]);
    return o;
}
static inline void unpack_fill(const std::vector<uint8_t>& payload,
                               std::vector<uint8_t>& fill_paths,
                               std::array<std::vector<uint8_t>,6>& mixedEncoded,
                               size_t mixedPartCount){
    const uint8_t* p = payload.data(); const uint8_t* e = p + payload.size();
    fill_paths = get_blob(p, e);
    for(size_t i=0;i<mixedPartCount;++i) mixedEncoded[i] = get_blob(p, e);
    if(p != e){ fprintf(stderr,"protocol: fill trailing bytes\n"); exit(2); }
}

}  // namespace capp
