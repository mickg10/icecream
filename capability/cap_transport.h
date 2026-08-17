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
// new work (per local-oracle's division of labor) is the ACTUAL transport and the
// explicit 128-bit generation framing.  This is that layer: a length-typed frame
// protocol over any stream fd (AF_UNIX socketpair for the prototype).  One opaque
// SourceGeneration is latched once, in the Hello frame, at conversation start; no
// per-object key rides the wire (that is the ruled generation-ordinal identity).
//
// Wire frame:  [u8 type][u32 little-endian length][length payload bytes]
// The payload is an opaque codec message; the transport does not parse it.
// -----------------------------------------------------------------------------
namespace cap {

enum class Frame : uint8_t { Hello = 0, Root = 1, Need = 2, Fill = 3, Done = 4, Ack = 5 };

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
    uint8_t hdr[5];
    hdr[0] = uint8_t(t);
    for (int i = 0; i < 4; ++i) hdr[1 + i] = uint8_t(len >> (8 * i));
    if (!write_all(fd, hdr, 5)) return false;
    return len == 0 || write_all(fd, data, len);
}
inline bool send_frame(int fd, Frame t, const std::vector<uint8_t>& d) {
    return send_frame(fd, t, d.data(), uint32_t(d.size()));
}

inline bool recv_frame(int fd, Frame& t, std::vector<uint8_t>& out) {
    uint8_t hdr[5];
    if (!read_all(fd, hdr, 5)) return false;
    t = Frame(hdr[0]);
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= uint32_t(hdr[1 + i]) << (8 * i);
    out.resize(len);
    return len == 0 || read_all(fd, out.data(), len);
}

// Latch the 128-bit generation once at conversation start.
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
