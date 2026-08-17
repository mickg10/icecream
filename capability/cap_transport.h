#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <errno.h>
#include "cap_identity.h"
// -----------------------------------------------------------------------------
// icecream #16 capability harness — the real two-process C<->F transport (M1).
//
// codec50 runs the whole ROOT -> F-derived NEED -> FILL dialogue in-process; M1's
// new work is the ACTUAL transport and the explicit 128-bit generation framing.
//
// Frame header (owner/local-oracle framing ruling, 2026-08-17): ONE packed 32-bit
// little-endian word — NOT the old [u8 type][u32 len] five-byte form:
//
//   header_le = (uint32(type) << 29) | payload_length
//   type      = header_le >> 29          (3 bits: 7 used types, value 7 reserved)
//   length    = header_le & 0x1fffffff   (29 bits, max 536,870,911 bytes)
//
// M2 (2026-08-17) adds Frame::Rejoin=6 (F->C worker restart/late-join signal); type 7
// stays reserved. Hello remains strictly C->F (128-bit generation latch).
//
// The real header is therefore exactly 4 bytes, matching codec50's charged FRAME=4 — but
// that zero-delta is ONLY the length prefix. It does NOT make the virtual category ledger
// equal the socket total: some codec payloads (ROOT, NEED, Block material, path defs) are
// sent RAW on the socket while the virtual ledger charges their z3-compressed size, and the
// relationship/job frames are extra real bytes the ledger omits (Hello = 20 B [4 header +
// 16-byte generation], empty Done/Ack = 4 B each). The category ledger is the codec
// accounting model + byte-exact gate; the socket total is the literal transfer, reported apart.
// -----------------------------------------------------------------------------
namespace cap {

enum class Frame : uint8_t { Hello = 0, Root = 1, Need = 2, Fill = 3, Done = 4, Ack = 5, Rejoin = 6 };
// Only types 0..6 are valid; the high 3-bit value 7 is reserved and rejected.
static constexpr uint32_t MAX_PAYLOAD = 0x1fffffffu;   // 29-bit payload cap = 536,870,911 B

// Pack (type,len) into the 32-bit header word. Rejects reserved types and over-length
// payloads so there is no silent truncation of a large vector::size().
inline bool pack_header(Frame t, uint32_t len, uint32_t& out) {
    uint32_t ty = uint32_t(t);
    if (ty > 6u || len > MAX_PAYLOAD) return false;
    out = (ty << 29) | len;
    return true;
}
inline bool unpack_header(uint32_t hdr, Frame& t, uint32_t& len) {
    uint32_t ty = hdr >> 29;
    if (ty > 6u) return false;                 // reject the single reserved type value (7)
    t = Frame(ty);
    len = hdr & MAX_PAYLOAD;
    return true;
}

inline bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= size_t(w);
    }
    return true;
}
inline bool read_all(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;   // peer closed
        p += r; n -= size_t(r);
    }
    return true;
}

inline bool send_frame(int fd, Frame t, const uint8_t* data, uint32_t len) {
    uint32_t h;
    if (!pack_header(t, len, h)) return false;
    uint8_t hdr[4];
    for (int i = 0; i < 4; ++i) hdr[i] = uint8_t(h >> (8 * i));   // little-endian
    if (!write_all(fd, hdr, 4)) return false;
    return len == 0 || write_all(fd, data, len);
}
inline bool send_frame(int fd, Frame t, const std::vector<uint8_t>& d) {
    if (d.size() > MAX_PAYLOAD) return false;   // enforce bound BEFORE the size() -> u32 cast
    return send_frame(fd, t, d.data(), uint32_t(d.size()));
}

inline bool recv_frame(int fd, Frame& t, std::vector<uint8_t>& out) {
    uint8_t hdr[4];
    if (!read_all(fd, hdr, 4)) return false;
    uint32_t h = 0;
    for (int i = 0; i < 4; ++i) h |= uint32_t(hdr[i]) << (8 * i);
    uint32_t len;
    if (!unpack_header(h, t, len)) return false;
    out.resize(len);
    return len == 0 || read_all(fd, out.data(), len);
}

// Latch the 128-bit generation once at conversation start. Hello on the wire = 20 bytes
// (4-byte header + 16-byte generation payload).
inline bool send_hello(int fd, const SourceGeneration& g) {
    return send_frame(fd, Frame::Hello, g.data(), 16);
}
inline bool recv_hello(int fd, SourceGeneration& g) {
    Frame t; std::vector<uint8_t> b;
    if (!recv_frame(fd, t, b) || t != Frame::Hello || b.size() != 16) return false;
    std::memcpy(g.data(), b.data(), 16);
    return true;
}

}  // namespace cap
