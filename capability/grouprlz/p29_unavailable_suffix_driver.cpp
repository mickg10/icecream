// Interactive Phase-B state seam used by p29_unavailable_suffix.sh.
//
// The process starts with no manifest and learns each TU path only through stdin.  It emits
// an Ack plus the unified event row before it will read the next path.  This is intentionally
// the small shared-C/F reference seam, not a claim that codec50-sink's manifest loader is
// already the final streaming producer.
#include "p29_two_f_state.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

p29::Opaque128 opaque(uint64_t seed) {
    p29::Opaque128 value{};
    for (size_t index = 0; index < value.size(); ++index) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        value[index] = static_cast<uint8_t>(seed * 0x2545f4914f6cdd1dULL);
    }
    return value;
}

p29::Digest128 digest_bytes(const std::vector<uint8_t>& bytes, uint64_t salt) {
    uint64_t lo = 1469598103934665603ULL ^ salt;
    uint64_t hi = 0x9e3779b97f4a7c15ULL + salt;
    for (uint8_t byte : bytes) {
        lo = (lo ^ byte) * 1099511628211ULL;
        hi = (hi + byte + 0x9e3779b97f4a7c15ULL) * 0xbf58476d1ce4e5b9ULL;
        hi = (hi << 17) | (hi >> 47);
    }
    return {lo, hi};
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open submitted TU: " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint32_t> publish_regions(p29::SharedCAuthority& authority,
                                      const std::vector<uint8_t>& bytes) {
    // Fixed-size exact chunks keep the fixture small while retaining repetitions for S1.
    // Length is part of std::string identity, so the final short chunk cannot alias a prefix.
    constexpr size_t chunk = 4;
    std::vector<std::string> atoms;
    atoms.reserve((bytes.size() + chunk - 1) / chunk);
    for (size_t begin = 0; begin < bytes.size(); begin += chunk) {
        const size_t length = std::min(chunk, bytes.size() - begin);
        atoms.emplace_back(reinterpret_cast<const char*>(bytes.data() + begin), length);
    }
    if (atoms.empty()) atoms.emplace_back();
    const std::vector<uint32_t> atom_ids = authority.publish_atoms(atoms).ids;
    std::vector<std::vector<uint32_t>> region_definitions;
    region_definitions.reserve(atom_ids.size());
    for (uint32_t atom : atom_ids) region_definitions.push_back({atom});
    return authority.publish_regions(region_definitions).ids;
}

}  // namespace

int main() {
    try {
        p29::SharedCAuthority authority(opaque(1), opaque(2));
        p29::FRouteEndpoint endpoint(authority, {opaque(3), 1}, 0);
        uint64_t sequence = 0;
        std::cout << "READY" << std::endl;

        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "END") break;
            static constexpr char prefix[] = "TU\t";
            if (command.compare(0, sizeof(prefix) - 1, prefix) != 0)
                throw std::runtime_error("expected TU<TAB>path or END");
            const std::string path = command.substr(sizeof(prefix) - 1);
            if (path.empty()) throw std::runtime_error("submitted TU path is empty");

            const std::vector<uint8_t> bytes = read_file(path);
            const std::vector<uint32_t> regions = publish_regions(authority, bytes);
            p29::SharedCAuthority::AdmissionInput input;
            input.producer_request_id = opaque(100 + sequence);
            input.source_extent = bytes.size();
            input.output_digest = digest_bytes(bytes, 11);
            input.regions = regions;
            const auto admission = authority.admit(input);
            if (admission.result != p29::SharedCAuthority::AdmissionResult::Created)
                throw std::runtime_error("interactive TU was not admitted exactly once");

            endpoint.prepare(admission.admitted,
                             {opaque(200), opaque(210 + sequence)});
            const p29::RouteCandidate selected =
                endpoint.candidate(p29::RepresentationMode::RouteS1);
            const p29::TransactionClosure closure{
                digest_bytes(bytes, 17 + selected.route_sequence),
                input.output_digest,
                input.source_extent};
            endpoint.begin_attempt(selected, closure);
            const p29::AckReceipt ack = endpoint.commit_at_f();
            endpoint.deliver_ack(ack);

            const p29::TuEventRecord& event = endpoint.events().back();
            const char* reason = nullptr;
            if (!event.validate(&reason))
                throw std::runtime_error(reason ? reason : "invalid interactive event");
            std::cout << "ACK\t" << sequence << '\t' << event.to_tsv() << std::endl;
            ++sequence;
        }
        if (sequence != 2 || endpoint.events().size() != 2)
            throw std::runtime_error("interactive suffix fixture requires exactly two Acked TUs");
        const auto mark = endpoint.state_mark();
        std::cout << "DONE\t" << sequence << '\t' << mark.c_route.committed_sequence
                  << '\t' << mark.c_mirror.mirror_sequence << '\t'
                  << mark.f_occurrence_digest << '\t' << mark.residency_digest << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "unavailable-suffix driver: " << error.what() << '\n';
        return 2;
    }
}
