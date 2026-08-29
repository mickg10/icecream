#include "client/p50_route_owner.h"
#include "cache/p50_control_operation.h"

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

P50RouteOwnerConfig config(ProfileId profile = ProfileId::Z3_LONG) {
    P50RouteOwnerConfig result;
    result.endpoint_caps.profile = profile;
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

void test_source_transfer_operation_wire() {
    local::P50SourceTransferRequest arm;
    arm.wire_job_id = 17;
    arm.assignment_epoch = 3;
    arm.assignment_nonce = 4;
    arm.selected_f_host = "127.0.0.1";
    arm.selected_f_ordinary_port = 8765;
    arm.selected_f_cache_port = 8766;
    arm.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    arm.cache_profile = CACHE_PROFILE_ZSTD_ROUTE;
    arm.logical_job = 5;
    arm.compiler_attempt = 6;
    arm.source_request_id = 8;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_ROUTE;
    CHECK(arm.valid());
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    const local::Identity identity{11, 12};
    const auto request = local::make_source_transfer_operation(identity, arm, deadline);
    const auto wire = local::encode_control_operation(request);
    CHECK(wire.size() == local::kSourceTransferOperationBytes);
    local::ControlOperation decoded;
    CHECK(local::decode_control_operation(wire, decoded));
    CHECK(decoded.kind == local::ControlOperationKind::SourceTransfer);
    CHECK(decoded.source_arm == arm);
    CHECK(decoded.absolute_deadline == deadline);

    // C control/store identity is supplied by the supervised sidecar, never
    // by this daemon request.  Any attempt to populate the reserved C slots
    // is rejected by the fixed operation decoder.
    auto spoofed = wire;
    spoofed[344] = 1;
    CHECK(!local::decode_control_operation(spoofed, decoded));

    local::P50SourceTransferResult committed;
    committed.code = local::SourceTransferResultCode::Committed;
    committed.attempts = 1;
    committed.tu_seq = 0; // The first route result is TU0.
    committed.raw_bytes = 3;
    committed.raw_digest.bytes[0] = 1;
    committed.c_store_guid.bytes[15] = 9;
    const auto reply = local::make_source_transfer_reply_operation(request, committed);
    const auto reply_wire = local::encode_control_operation(reply);
    CHECK(reply_wire.size() == local::kSourceTransferOperationBytes);
    CHECK(local::decode_control_operation(reply_wire, decoded));
    CHECK(decoded.source_result.has_value());
    CHECK(decoded.source_result->tu_seq == 0);
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

    // A different profile is also isolated.  ZSTD_TU retains only the
    // relationship's TU sequence; each payload is still compressed alone.
    const auto tu_route = relationship(101, 203, 1, ProfileId::ZSTD_TU);
    server.reset_store(Id128::from_u64(203));
    auto tu = route_call(context, owner, server, acceptor, tu_route, {7003, 1}, first);
    CHECK(tu.status == ZstdSourceTransferStatus::Committed);
    CHECK(tu.committed_input->tu_seq.value == 0);
    auto tu_successor = route_call(context, owner, server, acceptor, tu_route,
                                   {7003, 2}, second);
    CHECK(tu_successor.status == ZstdSourceTransferStatus::Committed);
    CHECK(tu_successor.committed_input->tu_seq.value == 1);
    CHECK(owner.owns(tu_route) && owner.owner_count() == 3);

    // Explicit F generation reset drops old route history; the new generation
    // recreates a sender at TU0.
    owner.reset_f_store(Id128::from_u64(201), 2);
    CHECK(!owner.owns(first_route) && owner.owner_count() == 2);
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

void test_p29_relationship_owner() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::P29;
    server_caps.supported_profiles = kOperationalProfileMask;
    P50ServerEndpoint server(Id128::from_u64(240), server_caps, nullptr, nullptr,
                             P50ServerEndpointConfig{
                                 .input_job_state = [](CStoreGuid, const TxBegin&,
                                                       const TxCommit&,
                                                       std::span<const uint8_t>) {
                                     return InputJobState::Open;
                                 }});
    P50CRouteOwner owner(config(ProfileId::P29));
    const auto route = relationship(141, 241, 1, ProfileId::P29);
    const std::vector<uint8_t> repeated{'p', '2', '9', '\n', 'p', '2', '9', '\n'};

    auto first = route_call(context, owner, server, acceptor, route, {7101, 1}, repeated);
    CHECK(first.status == ZstdSourceTransferStatus::Committed);
    CHECK(first.committed_input->tu_seq.value == 0);
    auto second = route_call(context, owner, server, acceptor, route, {7101, 2}, repeated);
    CHECK(second.status == ZstdSourceTransferStatus::Committed);
    CHECK(second.committed_input->tu_seq.value == 1);

    // A failed route attempt leaves the exact prepared request available for
    // the bounded retry path and cannot consume the next TU sequence.
    unsigned failed_connections = 0;
    context.restart();
    auto failed = asio::co_spawn(
        context,
        owner.transfer(route, {7101, 3},
                       ConnectedFdFactory{[&failed_connections](auto) {
                           ++failed_connections;
                           return -1;
                       }},
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       repeated),
        asio::use_future);
    context.run();
    CHECK(failed.get().status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_connections == 2);
    auto retried = route_call(context, owner, server, acceptor, route, {7101, 3}, repeated);
    CHECK(retried.status == ZstdSourceTransferStatus::Committed);
    CHECK(retried.committed_input->tu_seq.value == 2);

    // A different relationship is isolated and starts at TU0; an unsupported
    // profile fails closed without allocating an owner.
    const auto different = relationship(142, 241, 1, ProfileId::P29);
    auto isolated = route_call(context, owner, server, acceptor, different,
                               {7102, 1}, repeated);
    CHECK(isolated.status == ZstdSourceTransferStatus::Committed);
    CHECK(isolated.committed_input->tu_seq.value == 0);
    const auto unsupported = relationship(143, 241, 1, static_cast<ProfileId>(99));
    context.restart();
    auto rejected = asio::co_spawn(
        context,
        owner.transfer(unsupported, {7103, 1}, acceptor.local_endpoint(),
                        std::chrono::steady_clock::now() + std::chrono::seconds(10), repeated),
        asio::use_future);
    context.run();
    CHECK(rejected.get().status == ZstdSourceTransferStatus::InvalidRequest);

    // Resetting the F generation drops both P29 owners; the replacement starts
    // from TU0 and does not inherit either relationship's route history.
    owner.reset_f_store(Id128::from_u64(241), 2);
    CHECK(owner.owner_count() == 0);
    server.reset_store(Id128::from_u64(242));
    const auto replacement = relationship(141, 242, 2, ProfileId::P29);
    auto reset = route_call(context, owner, server, acceptor, replacement,
                            {7104, 1}, repeated);
    CHECK(reset.status == ZstdSourceTransferStatus::Committed);
    CHECK(reset.committed_input->tu_seq.value == 0);
}

#if defined(ICECC_P50_WITH_LIBBSC)
void test_grz_relationship_owner() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::GRZ;
    server_caps.supported_profiles = kOperationalProfileMask;
    P50ServerEndpoint server(Id128::from_u64(250), server_caps, nullptr, nullptr,
                             P50ServerEndpointConfig{
                                 .input_job_state = [](CStoreGuid, const TxBegin&,
                                                       const TxCommit&,
                                                       std::span<const uint8_t>) {
                                     return InputJobState::Open;
                                 }});
    P50CRouteOwner owner(config(ProfileId::GRZ));
    const auto route = relationship(151, 251, 1, ProfileId::GRZ);
    const std::vector<uint8_t> first{'g', 'r', 'z', '-', 't', 'u', '0', '\n'};
    const std::vector<uint8_t> second{'g', 'r', 'z', '-', 't', 'u', '1', '\n'};

    auto first_result = route_call(context, owner, server, acceptor, route,
                                   {7201, 1}, first);
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(first_result.committed_input->tu_seq.value == 0);
    auto second_result = route_call(context, owner, server, acceptor, route,
                                    {7201, 2}, second);
    CHECK(second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(second_result.committed_input->tu_seq.value == 1);
}
#endif

}  // namespace

int main() {
    test_source_transfer_operation_wire();
    test_long_lived_relationship_owner();
    test_relationship_validation();
    test_p29_relationship_owner();
#if defined(ICECC_P50_WITH_LIBBSC)
    test_grz_relationship_owner();
#endif
}
