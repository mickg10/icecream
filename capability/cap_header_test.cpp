// Pure pack_header/unpack_header tests for the packed 32-bit frame header
// (owner/local-oracle framing ruling). Covers: round-trip of all six types at
// lengths 0 and 0x1fffffff, rejection of over-limit lengths, and rejection of the
// two reserved type values (6, 7) on both pack and unpack.
#include "cap_transport.h"
#include <cstdio>
using namespace cap;

int main() {
    int fails = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); ++fails; }
    };

    const Frame types[6] = {Frame::Hello, Frame::Root, Frame::Need,
                            Frame::Fill,  Frame::Done, Frame::Ack};
    const uint32_t lens[2] = {0u, MAX_PAYLOAD};   // 0 and 0x1fffffff

    // round-trip every valid (type, length)
    for (Frame ty : types) for (uint32_t L : lens) {
        uint32_t h = 0;
        check(pack_header(ty, L, h), "pack valid type/len");
        Frame t2; uint32_t L2 = 0;
        check(unpack_header(h, t2, L2) && t2 == ty && L2 == L, "roundtrip type+len");
    }

    // the packed word really is 4 bytes, top 3 bits = type
    { uint32_t h = 0; pack_header(Frame::Fill, MAX_PAYLOAD, h);
      check((h >> 29) == uint32_t(Frame::Fill), "type in top 3 bits");
      check((h & MAX_PAYLOAD) == MAX_PAYLOAD, "length in low 29 bits"); }

    // reject over-limit lengths (no silent truncation)
    uint32_t hh = 0;
    check(!pack_header(Frame::Root, MAX_PAYLOAD + 1, hh), "reject len = max+1");
    check(!pack_header(Frame::Root, 0xFFFFFFFFu, hh),     "reject len = 4G-1");

    // reject the two reserved type values on pack
    check(!pack_header(Frame(6), 0, hh), "reject pack type 6");
    check(!pack_header(Frame(7), 0, hh), "reject pack type 7");

    // reject the two reserved type values on unpack
    Frame t; uint32_t L = 0;
    check(!unpack_header((6u << 29) | 10u, t, L), "reject unpack type 6");
    check(!unpack_header((7u << 29) | 10u, t, L), "reject unpack type 7");

    printf("cap_header_test (packed 4-byte header): %s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
