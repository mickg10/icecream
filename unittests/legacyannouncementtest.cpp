/* Deterministic round-trip regression for the pre-protocol-38 scheduler packet. */
#include "../services/legacyannouncement.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

#define REQUIRE(cond, what)                                                     \
    do {                                                                        \
        if (cond) {                                                             \
            fprintf(stderr, "ok       - %s\n", what);                         \
        } else {                                                                \
            fprintf(stderr, "FAILED   - %s (at %s:%d)\n", what, __FILE__,    \
                    __LINE__);                                                   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

struct DecodedLegacyAnnouncement {
    bool valid = false;
    unsigned char protocol = 0;
    uint64_t starttime = 0;
    unsigned int wire_length = 0;
    std::string netname;
};

static DecodedLegacyAnnouncement decode(const std::vector<char> &packet)
{
    using namespace LegacySchedulerAnnouncement;
    DecodedLegacyAnnouncement result;
    if (packet.size() < PayloadOffset + 1
            || packet[0] != 'I' || packet[1] != 'C' || packet[2] != 'E') {
        return result;
    }

    result.protocol = static_cast<unsigned char>(packet[3]);
    std::memcpy(&result.starttime, packet.data() + 4, sizeof(result.starttime));
    result.wire_length = static_cast<unsigned char>(packet[NetnameLengthOffset]);
    if (result.wire_length == 0
            || packet.size() != PayloadOffset + result.wire_length
            || packet.back() != '\0') {
        return result;
    }

    result.netname.assign(packet.data() + PayloadOffset, result.wire_length - 1);
    result.valid = true;
    return result;
}

int main()
{
    using namespace LegacySchedulerAnnouncement;
    const unsigned char protocol = 48;
    const time_t timestamp = static_cast<time_t>(0x12345678);

    const std::vector<char> ordinary_packet = encode(protocol, "ICECREAM", timestamp);
    const DecodedLegacyAnnouncement ordinary = decode(ordinary_packet);
    REQUIRE(ordinary.valid, "ordinary legacy announcement is structurally complete");
    REQUIRE(ordinary.protocol == protocol,
            "ordinary legacy announcement preserves the protocol version");
    REQUIRE(ordinary.starttime == static_cast<uint64_t>(timestamp),
            "ordinary legacy announcement preserves the native-endian start time");
    REQUIRE(ordinary.wire_length == 9,
            "ordinary legacy announcement length includes the trailing NUL");
    REQUIRE(ordinary.netname == "ICECREAM",
            "ordinary legacy announcement preserves the complete netname");

    const DecodedLegacyAnnouncement empty = decode(encode(protocol, nullptr, timestamp));
    REQUIRE(empty.valid && empty.wire_length == 1 && empty.netname.empty(),
            "empty legacy announcement has one representable NUL byte");

    std::string oversized;
    for (std::size_t i = 0; i < 300; ++i) {
        oversized += static_cast<char>('a' + (i % 26));
    }
    const DecodedLegacyAnnouncement maximum = decode(
        encode(protocol, oversized.c_str(), timestamp));
    REQUIRE(maximum.valid, "maximum legacy announcement is structurally complete");
    REQUIRE(maximum.wire_length == 255,
            "maximum legacy announcement uses the full unsigned one-byte length");
    REQUIRE(maximum.netname.size() == MaxNetnameBytes,
            "maximum legacy announcement carries exactly 254 netname bytes");
    REQUIRE(maximum.netname == oversized.substr(0, MaxNetnameBytes),
            "maximum legacy announcement truncates deterministically at the wire bound");

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
