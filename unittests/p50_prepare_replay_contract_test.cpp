#include "cache/p50_endpoint.h"

#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "p50_prepare_replay_contract_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

template<class Exception, class Function>
void require_throws(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(message) + " (wrong exception)");
    }
    fail(std::string(message) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

void test_lost_reply_replay_is_not_another_owner() {
    P50PreparationAuthority authority(Id128::from_u64(1));
    const PrepareRequestKey request{7, 91};
    const std::vector<uint8_t> input = bytes("one exact local admission\n");

    const PreparedTuHandle admitted = authority.prepare(request, input);
    const PreparedTuHandle replay = authority.prepare(request, input);

    require(admitted == replay,
            "replayed Prepare request returned another logical handle");
    require(authority.live_entry_count() == 1,
            "replayed Prepare request created another entry");

    // PrepareRequestKey denotes one logical admission. A replay caused by a
    // lost response must not acquire another reference. One eventual release
    // therefore reaches zero and reclaims the retained encoded TU.
    require(authority.release(replay) == 0,
            "lost-response Prepare replay acquired a phantom owner");
    require(authority.live_entry_count() == 0 &&
                authority.retained_encoded_bytes() == 0,
            "one release did not reclaim the idempotently replayed admission");
}

void test_explicit_retain_is_the_only_additional_owner() {
    P50PreparationAuthority authority(Id128::from_u64(2));
    const PreparedTuHandle handle = authority.prepare(
        PrepareRequestKey{8, 1}, bytes("explicit second owner\n"));

    require(authority.retain(handle) == 2,
            "explicit retain did not acquire a second owner");
    require(authority.release(handle) == 1,
            "first explicit-owner release did not leave one owner");
    require(authority.release(handle) == 0,
            "second explicit-owner release did not reach zero");
    require(authority.live_entry_count() == 0,
            "explicit retain/release left the admission resident");
}

void test_request_identity_is_typed_and_collision_safe() {
    P50PreparationAuthority authority(Id128::from_u64(3));
    const std::vector<uint8_t> first = bytes("same length payload A");
    const std::vector<uint8_t> second = bytes("same length payload B");
    require(first.size() == second.size(), "collision fixture lengths differ");

    const PrepareRequestKey request{9, 2};
    const PreparedTuHandle handle = authority.prepare(request, first);
    require_throws<std::invalid_argument>(
        [&] { (void)authority.prepare(request, second); },
        "Prepare request identity was rebound to different exact bytes");
    require(authority.release(handle) == 0,
            "collision fixture did not release its original admission");

    require_throws<std::invalid_argument>(
        [&] {
            (void)authority.prepare(PrepareRequestKey{0, 3}, first);
        },
        "zero producer-session identifier was accepted");
    require_throws<std::invalid_argument>(
        [&] {
            (void)authority.prepare(PrepareRequestKey{10, 0}, first);
        },
        "zero Prepare request token was accepted");
}

}  // namespace

int main() {
    test_lost_reply_replay_is_not_another_owner();
    test_explicit_retain_is_the_only_additional_owner();
    test_request_identity_is_typed_and_collision_safe();
    std::cout <<
        "p50_prepare_replay_contract_test: Prepare ownership contract passed\n";
    return 0;
}
