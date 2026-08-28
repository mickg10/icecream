#include "client/p50_route_owner.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <vector>

using namespace icecc::p50;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

P50RouteOwnerConfig config() {
    P50RouteOwnerConfig result;
    result.endpoint_caps.profile = ProfileId::Z3_LONG;
    result.endpoint_caps.supported_profiles = kOperationalProfileMask;
    result.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    result.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    result.maximum_duration = std::chrono::seconds(20);
    return result;
}

P50RouteRelationship relationship(uint64_t c, uint64_t f, uint64_t generation,
                                  ProfileId profile = ProfileId::Z3_LONG) {
    return P50RouteRelationship{Id128::from_u64(c), Id128::from_u64(f), generation,
                                profile};
}

ZstdSourceTransferResult route_call(
    asio::io_context& context, P50CRouteOwner& owner, P50ServerEndpoint& server,
    tcp::acceptor& acceptor, P50RouteRelationship route, PrepareRequestKey request,
    std::span<const uint8_t> source) {
    context.restart();
    auto server_result = asio::co_spawn(context, server.accept_one(acceptor),
                                         asio::use_future);
    auto transfer_result = asio::co_spawn(
        context,
        owner.transfer(route, request, acceptor.local_endpoint(),
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       source),
        asio::use_future);
    context.run();
    CHECK(server_result.get().status == ServerRunStatus::Completed);
    return transfer_result.get();
}

void test_long_lived_relationship_owner() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.supported_profiles = kOperationalProfileMask;
    P50ServerEndpoint server(Id128::from_u64(200), server_caps, nullptr, nullptr,
                             P50ServerEndpointConfig{
                                 .input_job_state = [](CStoreGuid, const TxBegin&,
                                                       const TxCommit&,
                                                       std::span<const uint8_t>) {
                                     return InputJobState::Open;
                                 }});
    P50CRouteOwner owner(config());
    const auto first_route = relationship(101, 201, 1);
    const std::vector<uint8_t> first{'t', 'u', '0'};
    const std::vector<uint8_t> second{'t', 'u', '1'};

    auto first_result = route_call(context, owner, server, acceptor, first_route,
                                   {7001, 1}, first);
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(first_result.committed_input->tu_seq.value == 0);
    auto second_result = route_call(context, owner, server, acceptor, first_route,
                                    {7001, 2}, second);
    CHECK(second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(second_result.committed_input->tu_seq.value == 1);
    CHECK(owner.owner_count() == 1 && owner.owns(first_route));

    // A reused request with different bytes is rejected before the route can
    // advance; the following exact successor is still TU2.
    context.restart();
    const std::vector<uint8_t> wrong{'w', 'r', 'o', 'n', 'g'};
    auto wrong_result = asio::co_spawn(
        context,
        owner.transfer(first_route, {7001, 2}, acceptor.local_endpoint(),
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       wrong),
        asio::use_future);
    context.run();
    CHECK(wrong_result.get().status == ZstdSourceTransferStatus::InvalidRequest);
    const std::vector<uint8_t> third{'t', 'u', '2'};
    auto third_result = route_call(context, owner, server, acceptor, first_route,
                                   {7001, 3}, third);
    CHECK(third_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(third_result.committed_input->tu_seq.value == 2);

    // A bounded connection failure is retried with the same preparation; it
    // cannot consume TU3 before a later exact retry commits.
    context.restart();
    unsigned failed_connections = 0;
    auto failed = asio::co_spawn(
        context,
        owner.transfer(
            first_route, {7001, 4},
            ConnectedFdFactory{[&failed_connections](auto) {
                ++failed_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            std::span<const uint8_t>(third)),
        asio::use_future);
    context.run();
    CHECK(failed.get().status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_connections == 2);
    auto fourth_result = route_call(context, owner, server, acceptor, first_route,
                                    {7001, 4}, third);
    CHECK(fourth_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(fourth_result.committed_input->tu_seq.value == 3);

    // A different F relationship gets an independent sender and TU0.
    const auto second_route = relationship(101, 202, 1);
    server.reset_store(Id128::from_u64(202));
    auto other_f = route_call(context, owner, server, acceptor, second_route,
                              {7002, 1}, first);
    CHECK(other_f.status == ZstdSourceTransferStatus::Committed);
    CHECK(other_f.committed_input->tu_seq.value == 0);
    CHECK(owner.owner_count() == 2);

    // A different profile is also isolated.  ZSTD_TU is deliberately
    // one-shot and is removed after its wrapper call.
    const auto tu_route = relationship(101, 203, 1, ProfileId::ZSTD_TU);
    server.reset_store(Id128::from_u64(203));
    auto tu = route_call(context, owner, server, acceptor, tu_route, {7003, 1}, first);
    CHECK(tu.status == ZstdSourceTransferStatus::Committed);
    CHECK(tu.committed_input->tu_seq.value == 0);
    CHECK(!owner.owns(tu_route) && owner.owner_count() == 2);

    // Explicit F generation reset drops old route history; the new generation
    // recreates a sender at TU0.
    owner.reset_f_store(Id128::from_u64(201), 2);
    CHECK(!owner.owns(first_route) && owner.owner_count() == 1);
    const auto replacement_route = relationship(101, 201, 2);
    server.reset_store(Id128::from_u64(201));
    auto replacement = route_call(context, owner, server, acceptor,
                                   replacement_route, {7004, 1}, first);
    CHECK(replacement.status == ZstdSourceTransferStatus::Committed);
    CHECK(replacement.committed_input->tu_seq.value == 0);
    owner.reset();
    CHECK(owner.owner_count() == 0);
}

void test_relationship_validation() {
    P50CRouteOwner owner(config());
    const P50RouteRelationship invalid{};
    asio::io_context context;
    const std::vector<uint8_t> source{'x'};
    auto result = asio::co_spawn(
        context,
        owner.transfer(invalid, {1, 1},
                       tcp::endpoint(asio::ip::address_v4::loopback(), 1),
                       std::chrono::steady_clock::now() + std::chrono::seconds(1),
                       source),
        asio::use_future);
    context.run();
    CHECK(result.get().status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(owner.owner_count() == 0);
}

}  // namespace

int main() {
    test_long_lived_relationship_owner();
    test_relationship_validation();
}
