#include "cache/p50_slice0.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_fill_closure_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

std::vector<uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::vector<std::vector<uint8_t>> regions(
    std::initializer_list<std::string_view> texts) {
    std::vector<std::vector<uint8_t>> result;
    for (std::string_view text : texts) result.push_back(bytes(text));
    return result;
}

void test_trailing_partial_fill_cannot_commit() {
    CAuthority c(Id128::from_u64(100));
    FStore f(Id128::from_u64(200));
    CRoute route(c, f.guid(), HistoryNonce{300});

    ReconnectResult connected = reconnect(route, f, HistoryNonce{301});
    require(connected.outcome == ReconnectOutcome::ColdFStore,
            "initial relationship was not cold");
    SessionHandle session = connected.session;

    const PreparedTUPtr prepared =
        c.prepare_from_regions(regions({"alpha\n", "beta\n", "gamma\n"}));
    const CActiveTx& active = route.begin(prepared);
    f.begin(session, active.begin);
    f.append_dict(session, active.dict);
    const Need need = f.need(session);
    require(!need.missing.empty(), "cold fixture unexpectedly has no Need");

    const std::vector<ImmutableObject> fill = route.build_fill(need);
    std::vector<FillRecord> records;
    records.reserve(fill.size());
    for (const ImmutableObject& object : fill)
        records.push_back(object.fill_record());

    std::vector<FillMessage> messages =
        encode_fill_messages(records, kInitialMaxFramePayload);
    require(!messages.empty(), "FILL fixture emitted no messages");

    for (size_t i = 0; i + 1 < messages.size(); ++i)
        (void)f.append_fill(session, messages[i]);

    // Every requested object closes normally, followed by one byte of a new,
    // incomplete record. Complete objects are durable, but this transaction
    // must not treat the malformed trailing byte as an empty FILL suffix.
    FillMessage malformed = messages.back();
    malformed.bytes.push_back(0);

    bool rejected = false;
    try {
        (void)f.append_fill(session, malformed);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    if (!rejected) {
        f.append_body(session, active.body);
        try {
            (void)f.materialize_and_verify(session);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
    }

    require(rejected,
            "transaction ignored a trailing partial FILL record");
    for (Key64 key : need.missing)
        require(f.contains(c.guid(), key),
                "complete object before malformed trailing FILL was rolled back");

    // Whole-TU replay starts with a fresh parser overlay but reuses every
    // completely installed immutable object.
    f.disconnect(session);
    const ReconnectResult resumed = reconnect(route, f, HistoryNonce{400});
    require(resumed.outcome == ReconnectOutcome::ExactMatch &&
                resumed.replay_active,
            "failed FILL did not retain C's replayable active transaction");
    session = resumed.session;
    f.begin(session, route.active()->begin, true);
    f.append_dict(session, route.active()->dict);
    require(f.need(session).missing.empty(),
            "replay requested completely installed FILL objects again");
    f.append_body(session, route.active()->body);
    require(f.materialize_and_verify(session) ==
                bytes("alpha\nbeta\ngamma\n"),
            "replay did not reconstruct exact input");
    route.accept_commit(f.commit_input(session));
}

}  // namespace

int main() {
    test_trailing_partial_fill_cannot_commit();
    std::cout << "p50_fill_closure_test: trailing FILL closure passed\n";
    return 0;
}
