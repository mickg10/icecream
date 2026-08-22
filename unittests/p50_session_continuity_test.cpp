#include "cache/p50_session_continuity.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_session_continuity_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Callable>
void require_reject(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }
    fail(std::string(text) + " (no exception)");
}

SessionHello offer() {
    SessionHello hello;
    hello.min_protocol = kProtocolVersion;
    hello.max_protocol = kProtocolVersion;
    hello.c_store_guid = Id128::from_u64(1);
    hello.supported_profiles = profile_bit(ProfileId::P29) |
                               profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {4096, 1U << 20};
    return hello;
}

SessionState staged_cold() {
    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.selected_profile = ProfileId::ZSTD_TU;
    state.limits = {2048, 1U << 18};
    state.f_store_guid = Id128::from_u64(2);
    return state;
}

HistoryReset requested(const SessionHello& hello) {
    const HistoryNonce nonce{17};
    return {nonce, initial_route_digest(hello.c_store_guid, nonce)};
}

SessionState acknowledgement(const SessionState& staged,
                             const HistoryReset& reset) {
    SessionState ack = staged;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{};
    ack.state_digest = reset.initial_state_digest;
    ack.last_commit.reset();
    return ack;
}

void test_exact_acknowledgement_passes() {
    const SessionHello hello = offer();
    const SessionState staged = staged_cold();
    const HistoryReset reset = requested(hello);
    const SessionState ack = acknowledgement(staged, reset);

    require(ack.f_store_guid == staged.f_store_guid &&
                ack.selected_profile == staged.selected_profile &&
                ack.limits == staged.limits,
            "valid acknowledgement fixture changed session identity");
    validate_initial_history_reset_ack(hello, staged, reset, ack);
}

void test_session_identity_cannot_change() {
    const SessionHello hello = offer();
    const SessionState staged = staged_cold();
    const HistoryReset reset = requested(hello);
    const SessionState good = acknowledgement(staged, reset);

    {
        SessionState changed = good;
        changed.f_store_guid = Id128::from_u64(99);
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement changed F_STORE_GUID");
    }
    {
        SessionState changed = good;
        // P29 is inside the original offer, so generic offer validation alone
        // accepts it. Continuity must still reject a second profile selection.
        changed.selected_profile = ProfileId::P29;
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement changed selected profile");
    }
    {
        SessionState changed = good;
        changed.limits.max_frame_payload /= 2;
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement renegotiated frame limit");
    }
    {
        SessionState changed = good;
        changed.limits.max_fill_record_bytes /= 2;
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement renegotiated FILL limit");
    }
}

void test_acknowledged_route_must_be_exact() {
    const SessionHello hello = offer();
    const SessionState staged = staged_cold();
    const HistoryReset reset = requested(hello);
    const SessionState good = acknowledgement(staged, reset);

    {
        SessionState changed = good;
        changed.history_nonce.value += 1;
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement changed HISTORY_NONCE");
    }
    {
        SessionState changed = good;
        changed.state_digest = icecc::digest128("wrong initial route");
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, changed);
            },
            "reset acknowledgement changed initial state digest");
    }
    {
        SessionState advanced = good;
        advanced.next_rel_seq = RelSeq{1};
        TxCommit commit;
        commit.history_nonce = advanced.history_nonce;
        commit.rel_seq = RelSeq{0};
        commit.tu_seq = TuSeq{1};
        commit.transaction_digest = icecc::digest128("tx");
        commit.raw_digest = icecc::digest128("raw");
        commit.post_state_digest = icecc::digest128("advanced");
        advanced.state_digest = commit.post_state_digest;
        advanced.last_commit = commit;
        // This is an intrinsically valid later route snapshot, but it is not
        // the acknowledgement of the requested zero-cursor reset.
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, reset, advanced);
            },
            "reset acknowledgement skipped directly to an advanced route");
    }
}

void test_request_and_staged_snapshot_must_be_canonical() {
    const SessionHello hello = offer();
    const SessionState staged = staged_cold();
    const HistoryReset reset = requested(hello);
    const SessionState good = acknowledgement(staged, reset);

    {
        HistoryReset bad = reset;
        bad.history_nonce = HistoryNonce{};
        bad.initial_state_digest = initial_route_digest(
            hello.c_store_guid, bad.history_nonce);
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, bad, good);
            },
            "initial reset used zero HISTORY_NONCE");
    }
    {
        HistoryReset bad = reset;
        bad.initial_state_digest = icecc::digest128("not canonical");
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, staged, bad, good);
            },
            "initial reset used a noncanonical digest");
    }
    {
        SessionState warm = acknowledgement(staged, reset);
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, warm, reset, good);
            },
            "initial reset started from an existing route");
    }
    {
        SessionHello zero = hello;
        zero.c_store_guid = CStoreGuid{};
        SessionState ack = good;
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    zero, staged, reset, ack);
            },
            "initial reset used zero C_STORE_GUID");
    }
    {
        SessionState zero = staged;
        zero.f_store_guid = FStoreGuid{};
        SessionState ack = acknowledgement(zero, reset);
        require_reject(
            [&] {
                validate_initial_history_reset_ack(
                    hello, zero, reset, ack);
            },
            "initial reset used zero F_STORE_GUID");
    }
}

}  // namespace

int main() {
    test_exact_acknowledgement_passes();
    test_session_identity_cannot_change();
    test_acknowledged_route_must_be_exact();
    test_request_and_staged_snapshot_must_be_canonical();
    std::cout <<
        "p50_session_continuity_test: initial reset continuity passed\n";
    return 0;
}
