#include "mo_factor_codec.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

std::vector<uint8_t> canonical(const std::vector<std::string>& originals,
                               const std::vector<std::string>& translations) {
    std::vector<mo_factor::Slice> os, ts;
    for (const auto& value : originals)
        os.push_back({reinterpret_cast<const uint8_t*>(value.data()), uint32_t(value.size())});
    for (const auto& value : translations)
        ts.push_back({reinterpret_cast<const uint8_t*>(value.data()), uint32_t(value.size())});
    std::vector<uint8_t> output;
    if (!mo_factor::build(os, ts, output)) std::abort();
    return output;
}
}  // namespace

int main() {
    // Sender commit is all-or-nothing even if a later prepared definition conflicts with an
    // established one after an earlier definition in that same batch was inserted.
    {
        mo_factor::EncoderState strong;
        mo_factor::Encoded seed; seed.pending_definitions = {"known"};
        check(strong.commit(seed), "sender strong-commit fixture seed failed");
        const auto before = strong.state_mark();
        mo_factor::Encoded conflicting; conflicting.pending_definitions = {"fresh", "known"};
        check(!strong.commit(conflicting), "partly conflicting sender batch was accepted");
        check(strong.state_mark() == before, "failed sender commit left a partial dictionary change");
        mo_factor::Encoded retry; retry.pending_definitions = {"fresh"};
        check(strong.commit(retry) && strong.size() == 2,
              "definition inserted before the failed commit was not rolled back for retry");
    }

    const auto member0 = canonical({"shared-name", "second"}, {"uno", "dos"});
    const auto member1 = canonical({"shared-name"}, {"tres"});
    const std::vector<mo_factor::Slice> members{
        {member0.data(), uint32_t(member0.size())},
        {member1.data(), uint32_t(member1.size())}};

    mo_factor::EncoderState encoder;
    mo_factor::DecoderState decoder;
    mo_factor::Encoded encoded;
    check(encoder.encode(members, encoded), "encoder prepare failed");
    check(encoded.pending_definitions.size() == 2, "fixture did not prepare two unique definitions");
    const auto c0 = encoder.state_mark();
    const auto f0 = decoder.state_mark();
    check(c0.values == 0 && f0.values == 0, "dictionary changed during encode preparation");

    std::vector<uint8_t> output;
    std::vector<uint32_t> lengths;
    decoder.begin_transaction();
    check(decoder.decode(encoded.control, encoded.definitions, encoded.translations,
                         encoded.ordinary, output, lengths),
          "decoder rejected the prepared frame");
    check(decoder.state_mark() != f0, "decoder transaction installed no tentative definitions");
    check(lengths == std::vector<uint32_t>({uint32_t(member0.size()), uint32_t(member1.size())}),
          "decoded member lengths differ");
    std::vector<uint8_t> expected = member0;
    expected.insert(expected.end(), member1.begin(), member1.end());
    check(output == expected, "decoded member bytes differ");
    decoder.abort_transaction();
    check(decoder.state_mark() == f0, "decoder abort did not restore the exact state mark");
    check(encoder.state_mark() == c0, "sender dictionary moved before Ack");

    // Retry the exact prepared bytes, commit F, then apply the sender definitions after Ack.
    decoder.begin_transaction();
    check(decoder.decode(encoded.control, encoded.definitions, encoded.translations,
                         encoded.ordinary, output, lengths),
          "decoder rejected the identical retry");
    decoder.commit_transaction();
    check(encoder.commit(encoded), "sender commit after Ack failed");
    check(encoder.size() == decoder.size() &&
              encoder.string_bytes() == decoder.string_bytes() &&
              encoder.content_digest() == decoder.content_digest(),
          "sender and receiver dictionary state differ after commit");
    check(encoded.pending_definitions.size() == 2 && encoded.pending_definitions[0] == "shared-name",
          "sender commit consumed the prepared retry bytes");

    // Once the strings are established, the next prepare carries no definitions and neither
    // state mark grows after the transaction.
    mo_factor::Encoded warm;
    check(encoder.encode(members, warm), "warm encoder prepare failed");
    check(warm.pending_definitions.empty(), "warm prepare resent established definitions");
    const auto cw = encoder.state_mark();
    const auto fw = decoder.state_mark();
    decoder.begin_transaction();
    check(decoder.decode(warm.control, warm.definitions, warm.translations, warm.ordinary,
                         output, lengths), "warm decoder failed");
    decoder.commit_transaction();
    check(encoder.commit(warm), "empty warm sender commit failed");
    check(encoder.state_mark() == cw && decoder.state_mark() == fw,
          "warm transaction changed dictionary state");

    bool threw = false;
    try { decoder.begin_transaction(); decoder.begin_transaction(); }
    catch (const std::logic_error&) { threw = true; }
    check(threw && decoder.has_pending_transaction(),
          "second decoder begin did not preserve the first transaction");
    decoder.abort_transaction();
    threw = false;
    try { decoder.commit_transaction(); } catch (const std::logic_error&) { threw = true; }
    check(threw, "decoder commit without a transaction was accepted");

    std::printf("MO factor prepare/abort/retry/Ack state %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
