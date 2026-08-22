#include "cache/p50_input_record.h"
#include "cache/p50_loopback.h"

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace icecc::p50;
using tcp = boost::asio::ip::tcp;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_record_endpoint_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(text) + " (wrong exception)");
    }
    fail(std::string(text) + " (no exception)");
}

std::vector<uint8_t> bytes(size_t count, uint32_t salt) {
    std::vector<uint8_t> result(count);
    for (size_t index = 0; index != result.size(); ++index)
        result[index] = static_cast<uint8_t>(
            (index * 79 + index / 31 + salt) & 0xff);
    return result;
}

std::vector<uint8_t> drain(InputCursor& cursor) {
    std::vector<uint8_t> result;
    std::array<uint8_t, 509> buffer{};
    while (!cursor.eof()) {
        const size_t count = cursor.read(buffer);
        require(count != 0, "non-EOF InputCursor made no progress");
        result.insert(result.end(), buffer.begin(), buffer.begin() + count);
    }
    return result;
}

struct ServerThread {
    explicit ServerThread(ZstdLoopbackServer& server)
        : thread([this, &server] {
              try {
                  server.serve_one_connection();
              } catch (...) {
                  error = std::current_exception();
              }
          }) {}

    ~ServerThread() {
        if (thread.joinable()) thread.join();
    }

    void join() {
        if (thread.joinable()) thread.join();
    }

    void require_success() {
        join();
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            fail(std::string("server unexpectedly failed: ") +
                 exception.what());
        } catch (...) {
            fail("server unexpectedly failed with a non-standard exception");
        }
    }

    template<class Exception>
    void require_failure() {
        join();
        if (!error) fail("server unexpectedly accepted failing publication");
        try {
            std::rethrow_exception(error);
        } catch (const Exception&) {
            return;
        } catch (...) {
            fail("server publication failed through the wrong exception");
        }
    }

    std::exception_ptr error;
    std::thread thread;
};

SessionHello hello(CStoreGuid c_guid, SessionLimits limits) {
    SessionHello result;
    result.c_store_guid = c_guid;
    result.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    result.limits = limits;
    return result;
}

ZstdTuEnvelope envelope_for(const SessionState& state, TuSeq tu_seq,
                            std::span<const uint8_t> input) {
    return encode_zstd_tu(state.history_nonce, state.next_rel_seq, tu_seq,
                          state.state_digest, input, 1);
}

void test_endpoint_commit_publishes_one_restartable_input() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(1000);
    const FStoreGuid f_guid = Id128::from_u64(1001);
    const SessionLimits limits{4096, 1U << 20};
    const std::vector<uint8_t> input = bytes(128 * 1024, 7);
    const TuSeq tu_seq{1002};
    const InputRecordKey key{c_guid, tu_seq};

    InputRecordStore store(4, 1U << 20);
    size_t publication_calls = 0;
    ZstdLoopbackConfig config;
    config.f_store_guid = f_guid;
    config.session_limits = limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin& begin, const TxCommit& commit,
            std::vector<uint8_t> exact_input) {
            ++publication_calls;
            require(store.publish(c_guid, begin, commit,
                                  std::move(exact_input)) ==
                        InputPublishResult::Published,
                    "endpoint did not publish one new InputRecord");
        });
    ServerThread running(server);

    ZstdLoopbackClient client(
        client_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, limits), 3);
    client.establish_initial_route(HistoryNonce{41});
    const ZstdTuEnvelope envelope = envelope_for(client.state(), tu_seq, input);
    const TxCommit commit = client.transfer(envelope, 127);
    client.close();
    running.require_success();

    require(publication_calls == 1 && store.contains(key) &&
                store.job_open(key),
            "one endpoint transaction did not create one open input lease");
    const SessionState route = server.route_snapshot();
    require(route.next_rel_seq.value == 1 && route.last_commit == commit,
            "InputRecord publication and route commit did not advance together");

    InputCursor failed_attempt = store.attach(key);
    std::array<uint8_t, 8192> prefix{};
    require(failed_attempt.read(prefix) == prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), input.begin()),
            "first compiler attempt read the wrong input prefix");
    failed_attempt = InputCursor{};

    // The replacement begins at byte zero from the same InputRecord. No client
    // reconnect/transfer or second route commit occurs in this interval.
    InputCursor replacement = store.attach(key);
    require(drain(replacement) == input,
            "replacement compiler did not restart at byte zero");
    require(publication_calls == 1 &&
                server.route_snapshot().next_rel_seq.value == 1,
            "compiler restart caused a second transfer or route advancement");
}

void test_store_capacity_failure_prevents_route_commit() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(2000);
    const SessionLimits limits{4096, 1U << 20};
    const std::vector<uint8_t> input = bytes(16 * 1024, 9);

    // This cap is deliberately below the exact input size.
    InputRecordStore store(1, 1024);
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(2001);
    config.session_limits = limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin& begin, const TxCommit& commit,
            std::vector<uint8_t> exact_input) {
            (void)store.publish(c_guid, begin, commit,
                                std::move(exact_input));
        });
    ServerThread running(server);

    ZstdLoopbackClient client(
        client_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, limits), 5);
    client.establish_initial_route(HistoryNonce{51});
    const ZstdTuEnvelope envelope =
        envelope_for(client.state(), TuSeq{2002}, input);
    require_throws<std::runtime_error>(
        [&] { (void)client.transfer(envelope, 97); },
        "InputRecord capacity failure was exposed as TX_COMMIT");
    client.close();
    running.require_failure<std::length_error>();

    const SessionState route = server.route_snapshot();
    require(route.route_present && route.next_rel_seq.value == 0 &&
                !route.last_commit && store.record_count() == 0 &&
                store.retained_bytes() == 0,
            "InputRecord capacity failure partially committed route/input state");
}

}  // namespace

int main() {
    test_endpoint_commit_publishes_one_restartable_input();
    test_store_capacity_failure_prevents_route_commit();
    std::cout <<
        "p50_input_record_endpoint_test: endpoint/restart integration passed\n";
    return 0;
}
