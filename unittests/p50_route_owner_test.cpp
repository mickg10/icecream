#include "client/p50_route_owner.h"
#include "cache/p50_control_operation.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>
#include <limits>
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

P50RouteOwnerConfig config(ProfileId profile = ProfileId::ZSTD_ROUTE) {
    P50RouteOwnerConfig result;
    result.endpoint_caps.profile = profile;
    result.endpoint_caps.supported_profiles = kOperationalProfileMask;
    result.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    result.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    result.maximum_duration = std::chrono::seconds(20);
    return result;
}

P50RouteRelationship relationship(uint64_t c, uint64_t f, uint64_t generation,
                                  ProfileId profile = ProfileId::ZSTD_ROUTE) {
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
    arm.cache_protocol = CACHE_WIRE_REVISION;
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

    local::P50SourceTransferResult replacement;
    replacement.code = local::SourceTransferResultCode::Error;
    replacement.error_code = static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired);
    replacement.attempts = 2;
    const auto replacement_reply =
        local::make_source_transfer_reply_operation(request, replacement);
    const auto replacement_wire = local::encode_control_operation(replacement_reply);
    CHECK(replacement_wire.size() == local::kSourceTransferOperationBytes);
    CHECK(local::decode_control_operation(replacement_wire, decoded));
    CHECK(decoded.source_result.has_value());
    CHECK(decoded.source_result->code == local::SourceTransferResultCode::Error);
    CHECK(decoded.source_result->error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));
    CHECK(decoded.source_result->attempts == 2);
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

void test_same_request_route_fork_wire() {
    P50PreparationAuthority authority(
        Id128::from_u64(190), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level, ProfileId::P29V1);
    const PreparationRouteKey route0{Id128::from_u64(290), 1, ProfileId::P29V1};
    const PreparationRouteKey route1{Id128::from_u64(291), 1, ProfileId::P29V1};
    const PrepareRequestKey fork_request{7900, 1};
    const std::vector<uint8_t> source{'f', 'o', 'r', 'k', '\n'};
    const auto h0 = authority.prepare_for_route(route0, fork_request, source);
    const auto h1 = authority.prepare_for_route(route1, fork_request, source);
    CHECK(authority.prepared_tu_seq(h0).value == authority.prepared_tu_seq(h1).value);
    CHECK(authority.prepared_profile(h0) == ProfileId::P29V1);
    CHECK(authority.prepared_profile(h1) == ProfileId::P29V1);

    asio::io_context context;
    tcp::acceptor acceptor0(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor acceptor1(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::P29V1;
    server_caps.supported_profiles = kOperationalProfileMask;
    server_caps.zstd = config().endpoint_caps.zstd;
    const P50ServerEndpointConfig server_config{
        .input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                              std::span<const uint8_t>) { return InputJobState::Open; }};
    P50ServerEndpoint server0(Id128::from_u64(290), server_caps, nullptr, nullptr,
                              server_config);
    P50ServerEndpoint server1(Id128::from_u64(291), server_caps, nullptr, nullptr,
                              server_config);
    ActionTrace actions0;
    ActionTrace actions1;
    // The endpoint must use the same authority as the prepared handles; build
    // the two route views with a shared C authority and retain independent
    // endpoint cursors for the wire proof.
    auto authority_ptr = std::shared_ptr<P50PreparationAuthority>(
        &authority, [](P50PreparationAuthority*) {});
    EndpointCaps mismatch_caps = server_caps;
    mismatch_caps.profile = ProfileId::ZSTD_TU;
    P50ClientEndpoint mismatch_endpoint(authority_ptr, mismatch_caps,
                                         HistoryNonce{1});
    asio::io_context mismatch_context;
    tcp::acceptor mismatch_acceptor(
        mismatch_context, {asio::ip::address_v4::loopback(), 0});
    auto mismatch_future = asio::co_spawn(
        mismatch_context,
        mismatch_endpoint.run(mismatch_acceptor.local_endpoint(), h0, {},
                               std::chrono::steady_clock::now() + std::chrono::seconds(10)),
        asio::use_future);
    mismatch_context.poll();
    CHECK(mismatch_future.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready);
    bool mismatch_rejected = false;
    try {
        (void)mismatch_future.get();
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }
    CHECK(mismatch_rejected);
    context.restart();
    P50ClientEndpoint endpoint0(authority_ptr, server_caps, HistoryNonce{1}, nullptr,
                                &actions0);
    P50ClientEndpoint endpoint1(authority_ptr, server_caps, HistoryNonce{1}, nullptr,
                                &actions1);
    auto server_future0 = asio::co_spawn(context, server0.accept_one(acceptor0), asio::use_future);
    auto server_future1 = asio::co_spawn(context, server1.accept_one(acceptor1), asio::use_future);
    auto client_future0 = asio::co_spawn(
        context, endpoint0.run(acceptor0.local_endpoint(), h0, {},
                               std::chrono::steady_clock::now() + std::chrono::seconds(10)),
        asio::use_future);
    auto client_future1 = asio::co_spawn(
        context, endpoint1.run(acceptor1.local_endpoint(), h1, {},
                               std::chrono::steady_clock::now() + std::chrono::seconds(10)),
        asio::use_future);
    context.run();
    const auto result0 = client_future0.get();
    const auto result1 = client_future1.get();
    CHECK(server_future0.get().status == ServerRunStatus::Completed);
    CHECK(server_future1.get().status == ServerRunStatus::Completed);
    CHECK(result0.status == ClientRunStatus::Committed);
    CHECK(result1.status == ClientRunStatus::Committed);
    CHECK(result0.committed_commit->tu_seq == result1.committed_commit->tu_seq);
    CHECK(result0.committed_commit->raw_digest == result1.committed_commit->raw_digest);
    CHECK(result0.committed_commit->rel_seq.value == 0);
    CHECK(result1.committed_commit->rel_seq.value == 0);
    CHECK(result0.committed_commit->transaction_digest != Digest128{});
    CHECK(result1.committed_commit->transaction_digest != Digest128{});
    CHECK(endpoint0.next_rel_seq().value == 1);
    CHECK(endpoint1.next_rel_seq().value == 1);
    CHECK(endpoint0.state_digest() == result0.committed_commit->post_state_digest);
    CHECK(endpoint1.state_digest() == result1.committed_commit->post_state_digest);
    CHECK(authority.release(h0) == 0);
    CHECK(authority.release(h1) == 0);
    CHECK(actions0.valid() && actions1.valid());
}

void test_multiroute_release_lifetime() {
    P50PreparationAuthority authority(
        Id128::from_u64(191), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level, ProfileId::P29V1);
    const PrepareRequestKey request{7901, 1};
    const std::vector<uint8_t> source{'m', 'u', 'l', 't', 'i', '-', 'r', 'o',
                                      'u', 't', 'e', '\n'};
    std::vector<PreparationRouteKey> routes;
    std::vector<PreparedTuHandle> handles;
    routes.reserve(20);
    handles.reserve(20);
    for (uint64_t index = 0; index != 20; ++index) {
        routes.push_back(
            {Id128::from_u64(300 + index), 1, ProfileId::P29V1});
        handles.push_back(authority.prepare_for_route(routes.back(), request, source));
        CHECK(authority.prepared_tu_seq(handles.back()).value == 0);
    }
    CHECK(authority.live_entry_count() == handles.size());
    const std::vector<size_t> release_order{
        19, 0, 18, 1, 17, 2, 16, 3, 15, 4,
        14, 5, 13, 6, 12, 7, 11, 8, 10, 9};
    for (size_t index : release_order)
        CHECK(authority.release(handles[index]) == 0);
    CHECK(authority.live_entry_count() == 0);
    CHECK(authority.retained_encoded_bytes() == 0);
    for (const auto& route : routes)
        CHECK(authority.reset_route(route));
}

void test_tu_seq_reservation_and_exhaustion() {
    ZstdTuLimits tight = config().endpoint_caps.zstd;
    tight.max_encoded_body_bytes = 32;
    P50PreparationAuthority authority(
        Id128::from_u64(192), tight, config().authority_limits,
        config().compression_level, ProfileId::ZSTD_TU);
    const PreparationRouteKey route{Id128::from_u64(392), 1,
                                    ProfileId::ZSTD_TU};
    const PrepareRequestKey request{7902, 1};
    std::vector<uint8_t> incompressible(4096);
    uint32_t state = 0x12345678U;
    for (auto& byte : incompressible) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>(state >> 24);
    }

    // Encoding fails after the C-wide sequence was reserved.  The failed
    // attempt leaves no request/entry behind, so the exact request can retry
    // and receives TU0; the next distinct admitted request receives TU1.
    bool encoding_failed = false;
    try {
        (void)authority.prepare_for_route(route, request, incompressible);
    } catch (const std::length_error&) {
        encoding_failed = true;
    }
    CHECK(encoding_failed);
    CHECK(authority.live_entry_count() == 0 &&
          authority.retained_encoded_bytes() == 0);
    const std::vector<uint8_t> retry_input{'r', 'e', 't', 'r', 'y'};
    const auto retry = authority.prepare_for_route(route, request, retry_input);
    CHECK(authority.prepared_tu_seq(retry).value == 0);
    CHECK(authority.release(retry) == 0);
    const auto next = authority.prepare_for_route(
        route, PrepareRequestKey{7902, 2}, std::span<const uint8_t>(retry_input));
    CHECK(authority.prepared_tu_seq(next).value == 1);
    CHECK(authority.release(next) == 0);

    // P29V1 builds its body before retained-byte admission. A body
    // that is too large for that admission bound therefore exercises the
    // post-encode rollback path; a smaller retry of the same request still
    // starts at the unconsumed TU0.
    PreparationAuthorityLimits post_limits = config().authority_limits;
    post_limits.max_retained_encoded_bytes = 64;
    P50PreparationAuthority post_admission(
        Id128::from_u64(194), config().endpoint_caps.zstd, post_limits,
        config().compression_level, ProfileId::P29V1);
    const PreparationRouteKey post_route{Id128::from_u64(394), 1,
                                         ProfileId::P29V1};
    std::vector<uint8_t> p29_many_regions;
    for (size_t index = 0; index != 256; ++index) {
        p29_many_regions.push_back('#');
        p29_many_regions.push_back(' ');
        p29_many_regions.push_back(static_cast<uint8_t>('A' + index % 26));
        p29_many_regions.push_back('\n');
    }
    bool post_admission_failed = false;
    try {
        (void)post_admission.prepare_for_route(
            post_route, PrepareRequestKey{7904, 1}, p29_many_regions);
    } catch (const std::length_error&) {
        post_admission_failed = true;
    }
    CHECK(post_admission_failed);
    CHECK(post_admission.live_entry_count() == 0 &&
          post_admission.retained_encoded_bytes() == 0);
    const std::vector<uint8_t> post_retry_input{'p', 'o', 's', 't', '\n'};
    const auto post_retry = post_admission.prepare_for_route(
        post_route, PrepareRequestKey{7904, 1}, post_retry_input);
    CHECK(post_admission.prepared_tu_seq(post_retry).value == 0);
    CHECK(post_admission.release(post_retry) == 0);

    // A nonzero start can be consumed exactly once.  A second route/profile
    // view of that same request uses the consumed identity, while a distinct
    // request fails closed at exhaustion.
    P50PreparationAuthority exhausted(
        Id128::from_u64(193), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level,
        ProfileId::ZSTD_TU, TuSeq{std::numeric_limits<uint64_t>::max()});
    const PreparationRouteKey zstd_route{Id128::from_u64(393), 1,
                                         ProfileId::ZSTD_TU};
    const PreparationRouteKey p29_route{Id128::from_u64(394), 1,
                                        ProfileId::P29V1};
    const std::vector<uint8_t> source{'m', 'a', 'x', '\n'};
    const auto max_zstd = exhausted.prepare_for_route(
        zstd_route, PrepareRequestKey{7903, 1}, source);
    const auto max_p29 = exhausted.prepare_for_route(
        p29_route, PrepareRequestKey{7903, 1}, source);
    CHECK(exhausted.prepared_tu_seq(max_zstd).value ==
          std::numeric_limits<uint64_t>::max());
    CHECK(exhausted.prepared_tu_seq(max_p29) ==
          exhausted.prepared_tu_seq(max_zstd));
    CHECK(exhausted.release(max_zstd) == 0);
    CHECK(exhausted.release(max_p29) == 0);
    bool exhausted_failed = false;
    try {
        (void)exhausted.prepare_for_route(
            zstd_route, PrepareRequestKey{7903, 2}, source);
    } catch (const std::overflow_error&) {
        exhausted_failed = true;
    }
    CHECK(exhausted_failed);
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
    const auto different_c = relationship(102, 201, 1);
    context.restart();
    auto rejected_c = asio::co_spawn(
        context,
        owner.transfer(different_c, {7000, 1}, acceptor.local_endpoint(),
                       std::chrono::steady_clock::now() + std::chrono::seconds(10), first),
        asio::use_future);
    context.run();
    CHECK(rejected_c.get().status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(owner.owner_count() == 1 && owner.owns(first_route));
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

    // A bounded connection failure poisons only its own relationship owner.
    // The exact retired F incarnation cannot be reset while its uncommitted
    // preparation is live, and no later wrapper may reopen F through it.
    P50CRouteOwner failed_owner(config());
    const auto failed_route = relationship(111, 211, 1);
    context.restart();
    unsigned failed_connections = 0;
    auto failed = asio::co_spawn(
        context,
        failed_owner.transfer(
            failed_route, {7011, 1},
            ConnectedFdFactory{[&failed_connections](auto) {
                ++failed_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            std::span<const uint8_t>(third)),
        asio::use_future);
    context.run();
    const auto failed_result = failed.get();
    CHECK(failed_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_result.replacement_required);
    CHECK(failed_connections == 2);
    unsigned poisoned_connections = 0;
    const auto other_failed_route = relationship(111, 212, 1);
    context.restart();
    auto poisoned = asio::co_spawn(
        context,
        failed_owner.transfer(
            other_failed_route, {7012, 1},
            ConnectedFdFactory{[&poisoned_connections](auto) {
                ++poisoned_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            std::span<const uint8_t>(third)),
        asio::use_future);
    context.run();
    const auto poisoned_result = poisoned.get();
    CHECK(poisoned_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(poisoned_result.replacement_required);
    CHECK(poisoned_result.route_local_failure);
    CHECK(poisoned_connections == 2);
    CHECK(failed_owner.owner_count() == 2 && failed_owner.owns(other_failed_route));
    context.restart();
    auto sticky = asio::co_spawn(
        context,
        failed_owner.transfer(
            other_failed_route, {7012, 2},
            ConnectedFdFactory{[&poisoned_connections](auto) {
                ++poisoned_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            std::span<const uint8_t>(third)),
        asio::use_future);
    context.run();
    CHECK(sticky.get().replacement_required);
    CHECK(poisoned_connections == 2);
    CHECK(!failed_owner.reset_f_store_exact(Id128::from_u64(211), 1));

    // The healthy relationship is independent and advances normally to TU3.
    auto fourth_result = route_call(context, owner, server, acceptor, first_route,
                                    {7001, 4}, third);
    CHECK(fourth_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(fourth_result.committed_input->tu_seq.value == 3);

    // A different F relationship gets an independent route, while TU_SEQ is
    // allocated from the one C authority and continues globally.
    const auto second_route = relationship(101, 202, 1);
    server.reset_store(Id128::from_u64(202));
    auto other_f = route_call(context, owner, server, acceptor, second_route,
                              {7002, 1}, first);
    CHECK(other_f.status == ZstdSourceTransferStatus::Committed);
    CHECK(other_f.committed_input->tu_seq.value == 4);
    CHECK(owner.owner_count() == 2);

    const auto cross_profile = relationship(101, 203, 1, ProfileId::ZSTD_TU);
    server.reset_store(Id128::from_u64(203));
    auto cross = route_call(context, owner, server, acceptor, cross_profile,
                            {7003, 1}, first);
    CHECK(cross.status == ZstdSourceTransferStatus::Committed);
    CHECK(cross.committed_input->tu_seq.value == 5);
    CHECK(owner.owner_count() == 3);

    // The same C-wide prepared record can be viewed through another profile
    // while the first view is live.  The route identities remain separate,
    // but the authenticated TU identity is shared.
    P50PreparationAuthority direct_authority(
        Id128::from_u64(180), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level,
        ProfileId::ZSTD_TU);
    const PreparationRouteKey direct_zstd{Id128::from_u64(280), 1,
                                          ProfileId::ZSTD_TU};
    const PreparationRouteKey direct_p29{Id128::from_u64(281), 1,
                                         ProfileId::P29V1};
    const PrepareRequestKey fork_request{7800, 1};
    const auto zstd_view = direct_authority.prepare_for_route(
        direct_zstd, fork_request, first);
    const auto p29_view = direct_authority.prepare_for_route(
        direct_p29, fork_request, first);
    CHECK(direct_authority.prepared_tu_seq(zstd_view).value == 0);
    CHECK(direct_authority.prepared_tu_seq(p29_view).value == 0);
    CHECK(direct_authority.prepared_profile(zstd_view) == ProfileId::ZSTD_TU);
    CHECK(direct_authority.prepared_profile(p29_view) == ProfileId::P29V1);
    CHECK(direct_authority.live_entry_count() == 2);
    CHECK(!direct_authority.reset_route(direct_zstd));
    CHECK(direct_authority.release(zstd_view) == 0);
    CHECK(direct_authority.release(p29_view) == 0);
    CHECK(direct_authority.reset_route(direct_zstd));

    P50PreparationAuthority p29_first_authority(
        Id128::from_u64(181), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level,
        ProfileId::P29V1);
    const auto p29_first = p29_first_authority.prepare_for_route(
        direct_p29, {7801, 1}, first);
    CHECK(p29_first_authority.prepared_tu_seq(p29_first).value == 0);
    CHECK(p29_first_authority.prepared_profile(p29_first) == ProfileId::P29V1);
    CHECK(p29_first_authority.release(p29_first) == 0);
    const auto zstd_after_p29 = p29_first_authority.prepare_for_route(
        direct_zstd, {7802, 1}, first);
    CHECK(p29_first_authority.prepared_tu_seq(zstd_after_p29).value == 1);
    CHECK(p29_first_authority.prepared_profile(zstd_after_p29) == ProfileId::ZSTD_TU);
    CHECK(p29_first_authority.release(zstd_after_p29) == 0);

    // Explicit F generation reset drops old route history; the replacement
    // route does not rewind the C-wide allocator.
    CHECK(owner.reset_f_store_exact(Id128::from_u64(201), 2));
    CHECK(owner.owns(first_route));
    CHECK(owner.reset_f_store_exact(Id128::from_u64(201), 1));
    CHECK(!owner.owns(first_route) && owner.owner_count() == 2);
    const auto replacement_route = relationship(101, 201, 2);
    server.reset_store(Id128::from_u64(201));
    auto replacement = route_call(context, owner, server, acceptor,
                                   replacement_route, {7004, 1}, first);
    CHECK(replacement.status == ZstdSourceTransferStatus::Committed);
    CHECK(replacement.committed_input->tu_seq.value == 6);
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

void test_p29v1_retry_and_reset_owner() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::P29V1;
    server_caps.supported_profiles = kOperationalProfileMask;
    P50ServerEndpoint server(Id128::from_u64(240), server_caps, nullptr, nullptr,
                             P50ServerEndpointConfig{
                                 .input_job_state = [](CStoreGuid, const TxBegin&,
                                                       const TxCommit&,
                                                       std::span<const uint8_t>) {
                                     return InputJobState::Open;
                                 }});
    P50CRouteOwner owner(config(ProfileId::P29V1));
    const auto route = relationship(141, 241, 1, ProfileId::P29V1);
    const std::vector<uint8_t> repeated{'p', '2', '9', '\n', 'p', '2', '9', '\n'};

    auto first = route_call(context, owner, server, acceptor, route, {7101, 1}, repeated);
    CHECK(first.status == ZstdSourceTransferStatus::Committed);
    CHECK(first.committed_input->tu_seq.value == 0);
    auto second = route_call(context, owner, server, acceptor, route, {7101, 2}, repeated);
    CHECK(second.status == ZstdSourceTransferStatus::Committed);
    CHECK(second.committed_input->tu_seq.value == 1);

    // A failed route attempt makes its isolated owner demand replacement;
    // the healthy owner remains available and allocates its next TU normally.
    P50CRouteOwner failed_owner(config(ProfileId::P29V1));
    const auto failed_route = relationship(151, 251, 1, ProfileId::P29V1);
    unsigned failed_connections = 0;
    context.restart();
    auto failed = asio::co_spawn(
        context,
        failed_owner.transfer(failed_route, {7151, 1},
                       ConnectedFdFactory{[&failed_connections](auto) {
                           ++failed_connections;
                           return -1;
                       }},
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       repeated),
        asio::use_future);
    context.run();
    const auto failed_result = failed.get();
    CHECK(failed_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_result.replacement_required);
    CHECK(failed_connections == 2);
    CHECK(!failed_owner.reset_f_store_exact(Id128::from_u64(251), 1));
    auto third = route_call(context, owner, server, acceptor, route, {7101, 3}, repeated);
    CHECK(third.status == ZstdSourceTransferStatus::Committed);
    CHECK(third.committed_input->tu_seq.value == 2);

    // A different F relationship in the same C store has an independent route
    // but shares TU_SEQ; an unsupported profile fails closed without allocating
    // an owner.  Different-C rejection is covered above without an accept.
    const auto different = relationship(141, 242, 1, ProfileId::P29V1);
    server.reset_store(Id128::from_u64(242));
    auto isolated = route_call(context, owner, server, acceptor, different,
                               {7102, 1}, repeated);
    CHECK(isolated.status == ZstdSourceTransferStatus::Committed);
    CHECK(isolated.committed_input->tu_seq.value == 3);
    const auto unsupported = relationship(143, 241, 1, static_cast<ProfileId>(99));
    context.restart();
    auto rejected = asio::co_spawn(
        context,
        owner.transfer(unsupported, {7103, 1}, acceptor.local_endpoint(),
                        std::chrono::steady_clock::now() + std::chrono::seconds(10), repeated),
        asio::use_future);
    context.run();
    CHECK(rejected.get().status == ZstdSourceTransferStatus::InvalidRequest);

    // Resetting the F generation drops both P29V1 route views; the replacement
    // starts fresh route history without rewinding TU_SEQ.
    CHECK(owner.reset_f_store_exact(Id128::from_u64(241), 2));
    CHECK(owner.owns(route));
    CHECK(owner.reset_f_store_exact(Id128::from_u64(241), 1));
    CHECK(owner.owner_count() == 1);
    server.reset_store(Id128::from_u64(243));
    const auto replacement = relationship(141, 243, 2, ProfileId::P29V1);
    auto reset = route_call(context, owner, server, acceptor, replacement,
                            {7104, 1}, repeated);
    CHECK(reset.status == ZstdSourceTransferStatus::Committed);
    CHECK(reset.committed_input->tu_seq.value == 4);
}

void test_relationship_table_cap_requests_replacement() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::ZSTD_ROUTE;
    server_caps.supported_profiles = kOperationalProfileMask;
    server_caps.zstd = config().endpoint_caps.zstd;
    P50ServerEndpoint server(
        Id128::from_u64(271), server_caps, nullptr, nullptr,
        P50ServerEndpointConfig{
            .input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                  std::span<const uint8_t>) {
                return InputJobState::Open;
            }});
    P50RouteOwnerConfig bounded = config();
    bounded.max_relationships = 1;
    P50CRouteOwner owner(bounded);
    const std::vector<uint8_t> source{'c', 'a', 'p'};
    const auto first_route = relationship(171, 271, 1);
    const auto first = route_call(context, owner, server, acceptor, first_route,
                                  {7401, 1}, source);
    CHECK(first.status == ZstdSourceTransferStatus::Committed);
    CHECK(owner.owner_count() == 1);

    unsigned connections = 0;
    context.restart();
    auto capped = asio::co_spawn(
        context,
        owner.transfer(
            relationship(171, 272, 1), {7402, 1},
            ConnectedFdFactory{[&connections](auto) {
                ++connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto capped_result = capped.get();
    CHECK(capped_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(capped_result.replacement_required);
    CHECK(connections == 0);
    CHECK(owner.owner_count() == 1 && owner.owns(first_route));
}

void test_transport_loss_does_not_reject_another_worker() {
    // H5: two workers share one C owner. A transport loss on F1 must not
    // reject F2 before even attempting its connection. Keep the failed route
    // retained; this is not permission to reset poisoned preparation state.
    P50CRouteOwner owner(config(ProfileId::P29V1));
    asio::io_context context;
    const std::vector<uint8_t> source{'h', '5'};
    const auto failed_route = relationship(191, 291, 1, ProfileId::P29V1);
    unsigned failed_connections = 0;
    auto first = asio::co_spawn(
        context, owner.transfer(failed_route, {7701, 1},
            ConnectedFdFactory{[&](auto) { ++failed_connections; return -1; }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    CHECK(first.get().status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_connections == 2);
    CHECK(owner.owns(failed_route));

    context.restart();
    unsigned other_connections = 0;
    auto other = asio::co_spawn(
        context, owner.transfer(relationship(191, 292, 1, ProfileId::P29V1),
            {7702, 1}, ConnectedFdFactory{[&](auto) {
                ++other_connections;
                return -1; // Count admission without a blocking test server.
            }}, std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto result = other.get();
    CHECK(other_connections == 2);
    CHECK(result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(owner.owns(failed_route));

    // A third relationship can actually commit after both retained failures,
    // without rewinding the C-wide TU allocator or dropping failed state.
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.supported_profiles = kOperationalProfileMask;
    P50ServerEndpoint server(Id128::from_u64(293), caps, nullptr, nullptr,
        P50ServerEndpointConfig{
            .input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                  std::span<const uint8_t>) {
                return InputJobState::Open;
            }});
    const auto healthy = relationship(191, 293, 1, ProfileId::P29V1);
    const auto committed = route_call(context, owner, server, acceptor, healthy,
                                      {7703, 1}, source);
    CHECK(committed.status == ZstdSourceTransferStatus::Committed);
    CHECK(committed.committed_input.has_value());
    CHECK(committed.committed_input->tu_seq.value == 2);
    CHECK(owner.owns(failed_route));
}

void test_typed_poison_catch_is_owner_wide() {
    P50RouteOwnerConfig injected = config(ProfileId::P29V1);
    unsigned injections = 0;
    injected.before_prepare_for_route_for_test = [&injections]() {
        ++injections;
        throw P50RoutePoisoned("injected begin_v1 terminalization");
    };
    P50CRouteOwner owner(std::move(injected));
    const auto first_route = relationship(181, 281, 1, ProfileId::P29V1);
    const std::vector<uint8_t> source{'p', 'o', 'i', 's', 'o', 'n'};

    unsigned first_connections = 0;
    asio::io_context context;
    auto first = asio::co_spawn(
        context,
        owner.transfer(
            first_route, {7501, 1},
            ConnectedFdFactory{[&first_connections](auto) {
                ++first_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto first_result = first.get();
    CHECK(first_result.status == ZstdSourceTransferStatus::TerminalError);
    CHECK(first_result.replacement_required);
    CHECK(injections == 1);
    CHECK(first_connections == 0);
    CHECK(owner.owner_count() == 1 && owner.owns(first_route));

    // The seam injects the typed result at the sender's prepare boundary; it
    // does not claim to exercise begin_v1's internal terminalization.  The
    // contract of P50RoutePoisoned says that terminalization already happened,
    // so its first observer must poison the whole C owner and refuse another F
    // before sender construction, preparation, or connection.
    unsigned successor_connections = 0;
    context.restart();
    auto successor = asio::co_spawn(
        context,
        owner.transfer(
            relationship(181, 282, 1, ProfileId::P29V1), {7502, 1},
            ConnectedFdFactory{[&successor_connections](auto) {
                ++successor_connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto successor_result = successor.get();
    CHECK(successor_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(successor_result.replacement_required);
    CHECK(injections == 1);
    CHECK(successor_connections == 0);
    CHECK(owner.owner_count() == 1);
}

void test_p29v1_relationship_owner() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::P29V1;
    server_caps.supported_profiles = kOperationalProfileMask;
    server_caps.zstd = config().endpoint_caps.zstd;
    P50ServerEndpoint server(Id128::from_u64(260), server_caps, nullptr, nullptr,
                             P50ServerEndpointConfig{
                                 .input_job_state = [](CStoreGuid, const TxBegin&,
                                                       const TxCommit&,
                                                       std::span<const uint8_t>) {
                                     return InputJobState::Open;
                                 }});

    // Keep the owner's primary/default profile at ZSTD_ROUTE.  The explicit
    // P29V1 relationship must lazily enable the shared C interner and retain
    // its route state without requiring a P29V1-only authority at startup.
    P50CRouteOwner owner(config());
    const auto route = relationship(161, 261, 1, ProfileId::P29V1);
    const std::vector<uint8_t> repeated{
        '#', ' ', '1', ' ', '"', 'a', '"', '\n',
        'p', '2', '9', 'v', '1', '\n'};

    const auto first = route_call(context, owner, server, acceptor, route,
                                  {7301, 1}, repeated);
    CHECK(first.status == ZstdSourceTransferStatus::Committed);
    CHECK(first.profile == ProfileId::P29V1);
    CHECK(first.committed_input->tu_seq.value == 0);
    CHECK(first.c_to_f_bytes > repeated.size());
    CHECK(first.f_to_c_bytes > 0);
    CHECK(first.system_source_reuse.has_value());
    const auto second = route_call(context, owner, server, acceptor, route,
                                   {7301, 2}, repeated);
    CHECK(second.status == ZstdSourceTransferStatus::Committed);
    CHECK(second.profile == ProfileId::P29V1);
    CHECK(second.committed_input->tu_seq.value == 1);
    CHECK(second.c_to_f_bytes > 0);
    CHECK(second.f_to_c_bytes > 0);
    CHECK(second.system_source_reuse == first.system_source_reuse);
    CHECK(owner.owner_count() == 1 && owner.owns(route));
}

// A TU interned before another TU's interner failure must not begin on a new
// route with a route logic error, which the sender escalates to a sticky
// sidecar replacement: it gets the same typed capability failure.
void test_interned_tu_after_interner_fault_is_typed() {
    P50RouteOwnerConfig owner_config = config(ProfileId::P29V1);
    P50PreparationAuthority authority(
        Id128::from_u64(193), owner_config.endpoint_caps.zstd,
        owner_config.authority_limits, owner_config.compression_level,
        ProfileId::P29V1, TuSeq{}, P29InternerFaultInjection::FailSecond);
    const std::vector<uint8_t> source{
        '#', ' ', '1', ' ', '"', 'k', 'e', 'p', 't', '"', '\n', 'o', 'n', 'e', '\n'};
    const std::vector<uint8_t> other{
        '#', ' ', '1', ' ', '"', 'l', 'o', 's', 't', '"', '\n', 't', 'w', 'o', '\n'};
    const PreparedTuHandle first = authority.prepare_for_route(
        {Id128::from_u64(391), 1, ProfileId::P29V1}, {7701, 1}, source);
    CHECK(authority.prepared_profile(first) == ProfileId::P29V1);

    bool failed_typed = false;
    try {
        (void)authority.prepare_for_route(
            {Id128::from_u64(392), 1, ProfileId::P29V1}, {7701, 2}, other);
    } catch (const P29V1CapabilityUnavailable&) {
        failed_typed = true;
    }
    CHECK(failed_typed);

    // The first TU's interned source is shared with a new route.
    bool reused_typed = false;
    try {
        (void)authority.prepare_for_route(
            {Id128::from_u64(393), 1, ProfileId::P29V1}, {7701, 1}, source);
    } catch (const P29V1CapabilityUnavailable&) {
        reused_typed = true;
    }
    CHECK(reused_typed);

    // A TU begun before the failure keeps its transaction and still writes
    // FILL and commits, and a ZSTD_TU route is unaffected.
    authority.pin_p29v1_system_source_reuse(
        first, authority.p29v1_system_source_fingerprint(first));
    CHECK(authority.fill_p29v1_before_need(first).has_value());
    authority.commit(first);
    const PreparedTuHandle zstd = authority.prepare_for_route(
        {Id128::from_u64(394), 1, ProfileId::ZSTD_TU}, {7701, 3}, other);
    CHECK(authority.prepared_profile(zstd) == ProfileId::ZSTD_TU);
    authority.commit(zstd);
}

// A HISTORY_RESET rebuilds the route's uncommitted successor at once, so a
// second reset of the same successor (its rebuilt TU was cut off before it
// reached F) still finds an active P29V1 transaction; a repeated nonce is no
// fresh history, and the successor still commits afterwards.
void test_consecutive_history_resets_of_one_successor() {
    P50PreparationAuthority authority(
        Id128::from_u64(210), config().endpoint_caps.zstd,
        config().authority_limits, config().compression_level, ProfileId::P29V1);
    const PreparationRouteKey route{Id128::from_u64(211), 1, ProfileId::P29V1};
    const std::vector<uint8_t> source{
        '#', ' ', '1', ' ', '"', 'r', '"', '\n', 'x', '\n'};
    const PreparedTuHandle successor =
        authority.prepare_for_route(route, {8101, 1}, source);
    CHECK(authority.reset_p29v1_route(successor, route.f_store_guid, HistoryNonce{501}) != nullptr);
    CHECK(authority.reset_p29v1_route(successor, route.f_store_guid, HistoryNonce{502}) != nullptr);
    bool repeated_rejected = false;
    try {
        (void)authority.reset_p29v1_route(successor, route.f_store_guid, HistoryNonce{502});
    } catch (const std::invalid_argument&) {
        repeated_rejected = true;
    }
    CHECK(repeated_rejected);
    authority.pin_p29v1_system_source_reuse(
        successor, authority.p29v1_system_source_fingerprint(successor));
    CHECK(authority.fill_p29v1_before_need(successor).has_value());
    authority.commit(successor);
}

void test_interner_fault_is_sticky_only_for_p29v1() {
    P50RouteOwnerConfig owner_config = config(ProfileId::P29V1);
    P50PreparationAuthority authority(
        Id128::from_u64(191), owner_config.endpoint_caps.zstd,
        owner_config.authority_limits, owner_config.compression_level,
        ProfileId::P29V1, TuSeq{}, P29InternerFaultInjection::FailOnce);
    const PreparationRouteKey p29_route{
        Id128::from_u64(291), 1, ProfileId::P29V1};
    const std::vector<uint8_t> source{
        '#', ' ', '1', ' ', '"', 'f', 'a', 'u', 'l', 't', '"', '\n',
        's', 'a', 'm', 'e', '\n', 's', 'a', 'm', 'e', '\n'};

    bool first_typed = false;
    try {
        (void)authority.prepare_for_route(p29_route, {7601, 1}, source);
    } catch (const P29V1CapabilityUnavailable&) {
        first_typed = true;
    }
    CHECK(first_typed);

    bool second_typed = false;
    try {
        (void)authority.prepare_for_route(p29_route, {7601, 2}, source);
    } catch (const P29V1CapabilityUnavailable&) {
        second_typed = true;
    }
    CHECK(second_typed);

    const PreparationRouteKey zstd_route{
        Id128::from_u64(292), 1, ProfileId::ZSTD_TU};
    const PreparedTuHandle zstd =
        authority.prepare_for_route(zstd_route, {7601, 3}, source);
    CHECK(authority.prepared_profile(zstd) == ProfileId::ZSTD_TU);
    CHECK(authority.prepared_tu_seq(zstd) == TuSeq{0});
    authority.commit(zstd);
    CHECK(authority.release(zstd) == 0);
}

}  // namespace

int main() {
    test_source_transfer_operation_wire();
    test_long_lived_relationship_owner();
    test_same_request_route_fork_wire();
    test_multiroute_release_lifetime();
    test_tu_seq_reservation_and_exhaustion();
    test_relationship_validation();
    test_p29v1_retry_and_reset_owner();
    test_p29v1_relationship_owner();
    test_relationship_table_cap_requests_replacement();
    test_transport_loss_does_not_reject_another_worker();
    test_typed_poison_catch_is_owner_wide();
    test_interner_fault_is_sticky_only_for_p29v1();
    test_interned_tu_after_interner_fault_is_typed();
    test_consecutive_history_resets_of_one_successor();
}
