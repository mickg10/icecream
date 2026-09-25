#include "client/p50_route_owner.h"
#include "cache/p50_control_operation.h"
#include "cache/p50_endpoint.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

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

void test_aborted_route_block_is_defined_on_other_f() {
    const auto caps_config = config(ProfileId::P29V1);
    P50PreparationAuthority authority(
        Id128::from_u64(195), caps_config.endpoint_caps.zstd,
        caps_config.authority_limits, caps_config.compression_level,
        ProfileId::P29V1);
    const PreparationRouteKey aborted_route{
        Id128::from_u64(295), 1, ProfileId::P29V1};
    const PreparationRouteKey live_route{
        Id128::from_u64(296), 1, ProfileId::P29V1};
    const std::vector<uint8_t> source{
        '#', ' ', '1', ' ', '"', 's', 'h', 'a', 'r', 'e', 'd', '.', 'h', '"', '\n',
        's', 'h', 'a', 'r', 'e', 'd', '-', 'b', 'l', 'o', 'c', 'k', '\n',
        's', 'h', 'a', 'r', 'e', 'd', '-', 'b', 'l', 'o', 'c', 'k', '\n',
        'l', 'i', 'v', 'e', '-', 'r', 'o', 'u', 't', 'e', '\n'};
    const PreparedTuHandle h_aborted = authority.prepare_for_route(
        aborted_route, {7802, 1}, source);
    const PreparedTuHandle h_live = authority.prepare_for_route(
        live_route, {7802, 2}, source);
    CHECK(authority.prepared_tu_seq(h_aborted) == TuSeq{0});
    CHECK(authority.prepared_tu_seq(h_live) == TuSeq{1});
    CHECK(authority.p29v1_interner_reserved_bytes() > 0);

    // Releasing an uncommitted route abandons its serializer state, but the
    // C-wide interner Block is shared by h_live and must remain usable.
    CHECK(authority.release(h_aborted) == 0);
    CHECK(!authority.contains(h_aborted));
    CHECK(authority.contains(h_live));
    CHECK(authority.live_entry_count() == 1);

    std::vector<uint8_t> reconstructed;
    unsigned job_state_calls = 0;
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::P29V1;
    server_caps.supported_profiles = kOperationalProfileMask;
    server_caps.zstd = caps_config.endpoint_caps.zstd;
    P50ServerEndpoint server(
        live_route.f_store_guid, server_caps, nullptr, nullptr,
        P50ServerEndpointConfig{
            .input_job_state = [&reconstructed, &job_state_calls](
                CStoreGuid, const TxBegin&, const TxCommit&,
                std::span<const uint8_t> exact_input) {
                ++job_state_calls;
                reconstructed.assign(exact_input.begin(), exact_input.end());
                return InputJobState::Open;
            }});
    ActionTrace actions;
    auto authority_ptr = std::shared_ptr<P50PreparationAuthority>(
        &authority, [](P50PreparationAuthority*) {});
    P50ClientEndpoint endpoint(authority_ptr, server_caps, HistoryNonce{1},
                               nullptr, &actions);
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    auto server_future = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    auto client_future = asio::co_spawn(
        context, endpoint.run(
                     acceptor.local_endpoint(), h_live, {},
                     std::chrono::steady_clock::now() + std::chrono::seconds(10)),
        asio::use_future);
    context.run();
    const auto client_result = client_future.get();
    CHECK(server_future.get().status == ServerRunStatus::Completed);
    CHECK(client_result.status == ClientRunStatus::Committed);
    CHECK(client_result.committed_commit->tu_seq == TuSeq{1});
    CHECK(reconstructed == source);
    CHECK(job_state_calls == 1);
    CHECK(actions.valid());
    CHECK(authority.release(h_live) == 0);

    // R2 incarnation retirement removes only the retired route's view. The
    // immutable C-wide request/interner object is still referenced by the
    // healthy route, so it remains reusable and the C TU allocator stays
    // monotonic.
    P50PreparationAuthority retired_authority(
        Id128::from_u64(196), caps_config.endpoint_caps.zstd,
        caps_config.authority_limits, caps_config.compression_level,
        ProfileId::P29V1);
    const PreparationRouteKey retired_route{
        Id128::from_u64(395), 1, ProfileId::P29V1};
    const PreparationRouteKey sibling_route{
        Id128::from_u64(396), 1, ProfileId::P29V1};
    const PrepareRequestKey shared_request{7803, 1};
    const auto retired_view = retired_authority.prepare_for_route(
        retired_route, shared_request, source);
    const auto sibling_view = retired_authority.prepare_for_route(
        sibling_route, shared_request, source);
    CHECK(retired_authority.live_entry_count() == 2);
    CHECK(retired_authority.prepared_tu_seq(retired_view) == TuSeq{0});
    CHECK(retired_authority.prepared_tu_seq(sibling_view) == TuSeq{0});
    CHECK(retired_authority.abandon_retired_route(retired_route));
    CHECK(!retired_authority.contains(retired_view));
    CHECK(retired_authority.contains(sibling_view));
    CHECK(retired_authority.live_entry_count() == 1);
    const auto same_request_again = retired_authority.prepare_for_route(
        sibling_route, shared_request, source);
    CHECK(retired_authority.prepared_tu_seq(same_request_again) == TuSeq{0});
    CHECK(retired_authority.p29v1_interner_reserved_bytes() > 0);
    const PreparationRouteKey successor_route{
        Id128::from_u64(397), 1, ProfileId::P29V1};
    const auto next_request = retired_authority.prepare_for_route(
        successor_route, {7803, 2}, source);
    CHECK(retired_authority.prepared_tu_seq(next_request) == TuSeq{1});
    CHECK(retired_authority.abandon_retired_route(sibling_route));
    CHECK(retired_authority.abandon_retired_route(successor_route));
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
    CHECK(capped_result.replacement_trigger ==
          ReplacementTrigger::Unattributed);
    CHECK(connections == 0);
    CHECK(owner.owner_count() == 1 && owner.owns(first_route));

    context.restart();
    auto later = asio::co_spawn(
        context,
        owner.transfer(
            relationship(171, 273, 1), {7403, 1},
            ConnectedFdFactory{[&connections](auto) {
                ++connections;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto later_result = later.get();
    CHECK(later_result.replacement_required);
    CHECK(later_result.replacement_trigger ==
          ReplacementTrigger::Unattributed);
    CHECK(connections == 0);
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

void test_typed_poison_does_not_destroy_another_active_route() {
    P50RouteOwnerConfig injected = config(ProfileId::P29V1);
    unsigned prepare_calls = 0;
    injected.before_prepare_for_route_for_test = [&prepare_calls]() {
        if (++prepare_calls == 2)
            throw P50RoutePoisoned("injected C-wide terminalization");
    };
    P50CRouteOwner owner(std::move(injected));
    const auto active_route = relationship(183, 283, 1, ProfileId::P29V1);
    const auto poisoned_route = relationship(183, 284, 1, ProfileId::P29V1);
    const auto refused_route = relationship(183, 285, 1, ProfileId::P29V1);
    const std::vector<uint8_t> source{'a', 'c', 't', 'i', 'v', 'e', '\n'};
    asio::io_context context;

    std::function<void(int)> release_active_connect;
    unsigned active_connect_calls = 0;
    AsyncConnectedFdFactory held_connection =
        [&release_active_connect, &active_connect_calls](
            auto, std::function<void(int)> completion) {
            ++active_connect_calls;
            if (active_connect_calls == 1)
                release_active_connect = std::move(completion);
            else
                completion(-1);
        };
    auto active = asio::co_spawn(
        context,
        owner.transfer(active_route, {7503, 1}, std::move(held_connection),
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       source),
        asio::use_future);
    context.poll();
    CHECK(active_connect_calls == 1);
    CHECK(static_cast<bool>(release_active_connect));
    CHECK(active.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

    AsyncConnectedFdFactory unused_poison_connection =
        [](auto, std::function<void(int)> completion) { completion(-1); };
    auto poisoned = asio::co_spawn(
        context,
        owner.transfer(poisoned_route, {7503, 2},
                       std::move(unused_poison_connection),
                       std::chrono::steady_clock::now() + std::chrono::seconds(10),
                       source),
        asio::use_future);
    context.poll();
    CHECK(poisoned.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const auto poison_result = poisoned.get();
    CHECK(poison_result.status == ZstdSourceTransferStatus::TerminalError);
    CHECK(poison_result.replacement_required);
    CHECK(prepare_calls == 2);
    CHECK(active.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    CHECK(owner.owns(active_route) && owner.owns(poisoned_route));

    unsigned refused_connection_calls = 0;
    auto refused = asio::co_spawn(
        context,
        owner.transfer(
            refused_route, {7503, 3},
            ConnectedFdFactory{[&refused_connection_calls](auto) {
                ++refused_connection_calls;
                return -1;
            }},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.poll();
    CHECK(refused.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const auto refused_result = refused.get();
    CHECK(refused_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(refused_result.replacement_required);
    CHECK(refused_connection_calls == 0);
    CHECK(owner.owner_count() == 2);

    // The route-owner unit stops at this layer: keep the captured coroutine's
    // sender/authority alive until its exact operation settles, then join it.
    // It does not assert the supervised Runtime's no-retry fence or shutdown.
    release_active_connect(-1);
    release_active_connect = {};
    context.run();
    CHECK(active.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    const auto active_result = active.get();
    CHECK(active_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(active_result.route_local_failure);
    CHECK(active_connect_calls == 2);
    CHECK(owner.owns(active_route) && owner.owns(poisoned_route));
    CHECK(owner.owner_count() == 2);
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

asio::awaitable<ServerRunResult> accept_topology_links(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint, size_t count) {
    auto executor = co_await asio::this_coro::executor;
    std::vector<tcp::socket> sockets;
    sockets.reserve(count);
    for (size_t i = 0; i != count; ++i) {
        tcp::socket socket(executor);
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        sockets.push_back(std::move(socket));
    }
    struct Completion {
        std::mutex mutex;
        size_t finished = 0;
        std::vector<ServerRunResult> results;
    };
    auto completion = std::make_shared<Completion>();
    completion->results.resize(count);
    for (size_t i = 0; i != count; ++i) {
        asio::co_spawn(executor, endpoint.run_adopted_r2(std::move(sockets[i])),
            [completion, i](std::exception_ptr error, ServerRunResult result) {
                std::lock_guard lock(completion->mutex);
                completion->results[i] = error
                    ? ServerRunResult{ServerRunStatus::TerminalError}
                    : std::move(result);
                ++completion->finished;
            });
    }
    asio::steady_timer timer(executor);
    for (;;) {
        {
            std::lock_guard lock(completion->mutex);
            if (completion->finished == count) break;
        }
        timer.expires_after(std::chrono::milliseconds(5));
        co_await timer.async_wait(asio::use_awaitable);
    }
    for (const auto& result : completion->results)
        if (result.status != ServerRunStatus::Disconnected)
            co_return result;
    co_return ServerRunResult{ServerRunStatus::Disconnected, {}};
}

struct TopologyLink {
    CStoreGuid c_guid{};
    FStoreGuid f_guid{};
    Id128 relationship{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_generation = 0;
    uint64_t c_generation = 0;
    uint64_t f_generation = 0;
    uint64_t control_generation = 0;
    uint64_t control_attempt = 0;
    std::vector<P51SourceArmedFields> armed;
    std::vector<std::vector<uint8_t>> input;
    std::shared_ptr<P50PreparationAuthority> authority;
    std::shared_ptr<P50ZstdSourceSender> sender;
    std::atomic<unsigned> sent{0};
    std::atomic<unsigned> committed{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<unsigned> connector_calls{0};
};

std::array<uint8_t, 16> topology_guid(uint8_t seed, bool file_role) {
    std::array<uint8_t, 16> bytes{};
    for (size_t i = 0; i != bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(seed + i);
    if (file_role)
        bytes[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    else
        bytes[kStoreIdentityRoleByte] &= static_cast<uint8_t>(~kStoreIdentityRoleMask);
    return bytes;
}

uint32_t topology_cache_profile(ProfileId profile) {
    switch (profile) {
    case ProfileId::P29V1: return CACHE_PROFILE_P29V1;
    case ProfileId::ZSTD_TU: return CACHE_PROFILE_ZSTD_TU;
    case ProfileId::ZSTD_ROUTE: return CACHE_PROFILE_ZSTD_ROUTE;
    }
    return 0;
}

uint32_t topology_source_mode(ProfileId profile) {
    switch (profile) {
    case ProfileId::P29V1: return P50_SOURCE_MODE_P29V1;
    case ProfileId::ZSTD_TU: return P50_SOURCE_MODE_ZSTD_TU;
    case ProfileId::ZSTD_ROUTE: return P50_SOURCE_MODE_ZSTD_ROUTE;
    }
    return 0;
}

const char* topology_profile_name(ProfileId profile) {
    switch (profile) {
    case ProfileId::P29V1: return "P29V1";
    case ProfileId::ZSTD_TU: return "ZSTD_TU";
    case ProfileId::ZSTD_ROUTE: return "ZSTD_ROUTE";
    }
    return "invalid";
}

P51SourceArmFields topology_arm(const TopologyLink& link, uint32_t wire_job,
                                ProfileId profile) {
    P51SourceArmFields arm;
    arm.source.wire_job_id = wire_job;
    arm.source.assignment_epoch = 3;
    arm.source.assignment_nonce = static_cast<uint64_t>(wire_job) + 1000;
    arm.source.selected_f_host = "127.0.0.1";
    arm.source.selected_f_ordinary_port = 42001;
    arm.source.selected_f_cache_port = 42002;
    arm.source.cache_protocol = 2;
    arm.source.cache_profile = topology_cache_profile(profile);
    arm.source.logical_job = 700 + wire_job;
    arm.source.compiler_attempt = 900 + wire_job;
    arm.source.c_store_generation = link.c_generation;
    arm.source.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.source.c_store_guid = link.c_guid.bytes;
    arm.source.source_request_id = static_cast<uint64_t>(wire_job) + 1000;
    arm.source.source_mode = topology_source_mode(profile);
    arm.source.c_control_generation = link.control_generation;
    arm.source.c_control_attempt = link.control_attempt;
    arm.requested_window = 30;
    return arm;
}

P51SourceArmedFields topology_armed(const TopologyLink& link,
                                    P51SourceArmFields arm, size_t ordinal) {
    P51SourceArmedFields armed;
    armed.arm = std::move(arm);
    armed.f_control_generation = 300 + link.f_generation;
    armed.f_control_attempt = 400 + link.f_generation;
    armed.f_store_generation = link.f_generation;
    armed.f_store_guid = link.f_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 500 + ordinal;
    armed.source_budget_msec = 25000;
    for (size_t i = 0; i != 16; ++i) {
        armed.attempt_capability_1.bytes[i] = static_cast<uint8_t>(17 + i);
        armed.attempt_capability_2.bytes[i] = static_cast<uint8_t>(47 + i);
    }
    armed.reservation_id = Id128::from_u64(0x700000 + ordinal).bytes;
    armed.logical_relationship_id = link.relationship.bytes;
    armed.relationship_epoch = link.relationship_epoch;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    return armed;
}

void test_p51_route_owner_preserves_precise_capacity_trigger() {
    constexpr ProfileId profile = ProfileId::ZSTD_ROUTE;
    TopologyLink link;
    link.c_guid = CStoreGuid{topology_guid(41, false)};
    link.f_guid = FStoreGuid{topology_guid(101, true)};
    link.relationship = Id128::from_u64(0x7182);
    link.relationship_epoch = 1;
    link.physical_generation = 77;
    link.c_generation = 78;
    link.f_generation = 79;
    link.control_generation = 80;
    link.control_attempt = 81;
    auto first_arm = topology_arm(link, 501, profile);
    auto second_arm = topology_arm(link, 502, profile);
    const auto first_armed = topology_armed(link, std::move(first_arm), 1);
    const auto second_armed = topology_armed(link, std::move(second_arm), 2);
    const PrepareRequestKey first_request{
        first_armed.arm.source.assignment_epoch,
        first_armed.arm.source.source_request_id};
    const PrepareRequestKey second_request{
        second_armed.arm.source.assignment_epoch,
        second_armed.arm.source.source_request_id};
    const P50RouteRelationship route{link.c_guid, link.f_guid,
                                     link.f_generation, profile};
    const std::vector<uint8_t> first_source{'f', 'i', 'r', 's', 't'};
    const std::vector<uint8_t> second_source{'s', 'e', 'c', 'o', 'n', 'd'};

    P50RouteOwnerConfig owner_config = config(profile);
    owner_config.max_completed_requests = 1;
    owner_config.authority_limits.max_speculative_tus = 1;
    owner_config.authority_limits.max_speculative_raw_bytes = 1U << 20;
    owner_config.authority_limits.max_live_entries = 4;
    P50CRouteOwner owner(std::move(owner_config));

    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_caps.zstd = config(profile).endpoint_caps.zstd;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto sidecar_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            deadline, clock.clock_domain_id, clock.time_namespace_id);
    P50ServerEndpointConfig server_config;
    server_config.lookup_p51_link_reservation =
        [&, sidecar_deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.relationship_id != link.relationship ||
            hello.c_store_guid != link.c_guid ||
            hello.f_store_guid != link.f_guid ||
            hello.f_store_generation != link.f_generation)
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = first_armed;
        lease.absolute_deadline = sidecar_deadline;
        lease.relationship_epoch = hello.relationship_epoch;
        lease.history_nonce = hello.history_nonce;
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, sidecar_deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (hello.relationship_id != link.relationship ||
            binding.reservation_id != Id128{first_armed.reservation_id} ||
            binding.raw_bytes != first_source.size() ||
            binding.raw_digest != icecc::digest128(first_source))
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = first_armed;
        lease.absolute_deadline = sidecar_deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{link.c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello&, const JobBind&, const R2TxCommit& commit) {
        return commit.inner.raw_digest == icecc::digest128(first_source);
    };
    server_config.acknowledge_p51_receipt =
        [](const LinkHello&, const CommitAck&) { return true; };
    P50ServerEndpoint server(link.f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));

    asio::io_context context;
    tcp::acceptor acceptor(
        context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
    auto accept_once = [&]() -> asio::awaitable<ServerRunResult> {
        tcp::socket socket(co_await asio::this_coro::executor);
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        co_return co_await server.run_adopted_r2(std::move(socket));
    };
    auto server_future = asio::co_spawn(context, accept_once(), asio::use_future);
    auto work = asio::make_work_guard(context);
    std::thread io_thread([&] { context.run(); });
    struct Cleanup {
        asio::io_context& context;
        asio::executor_work_guard<asio::io_context::executor_type>& work;
        std::thread& thread;
        ~Cleanup() {
            work.reset();
            context.stop();
            if (thread.joinable()) thread.join();
        }
    } cleanup{context, work, io_thread};

    const tcp::endpoint remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector = [remote](auto, auto completion) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { completion(-1); return; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(remote.port());
        const auto ip = remote.address().to_v4().to_bytes();
        std::memcpy(&address.sin_addr, ip.data(), ip.size());
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0) {
            (void)::close(fd);
            completion(-1);
            return;
        }
        completion(fd);
    };
    auto first = asio::co_spawn(
        context, owner.transfer_p51(route, first_armed, connector,
                                    first_request, deadline, first_source),
        asio::use_future);
    CHECK(first.wait_until(deadline) == std::future_status::ready);
    CHECK(first.get().status == ZstdSourceTransferStatus::Committed);

    auto full = asio::co_spawn(
        context, owner.transfer_p51(route, second_armed, connector,
                                    second_request, deadline, second_source),
        asio::use_future);
    CHECK(full.wait_until(deadline) == std::future_status::ready);
    const auto full_result = full.get();
    CHECK(full_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(full_result.replacement_required);
    CHECK(full_result.replacement_trigger ==
          ReplacementTrigger::CompletedRequestCapacity);

    auto sticky = asio::co_spawn(
        context, owner.transfer_p51(route, second_armed, connector,
                                    second_request, deadline, second_source),
        asio::use_future);
    CHECK(sticky.wait_until(deadline) == std::future_status::ready);
    const auto sticky_result = sticky.get();
    CHECK(sticky_result.replacement_required);
    CHECK(sticky_result.replacement_trigger ==
          ReplacementTrigger::CompletedRequestCapacity);

    std::promise<bool> retired_promise;
    auto retired = retired_promise.get_future();
    asio::post(context, [&] {
        retired_promise.set_value(owner.retire_f_store_exact_p51(
            link.f_guid, link.f_generation));
    });
    CHECK(retired.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(retired.get());
    CHECK(server_future.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
}

void test_p51_w30_direct_topology(size_t c_count, size_t f_count,
                                  ProfileId profile) {
    constexpr size_t kWindow = 30;
    constexpr size_t kRefill = 31;
    std::fprintf(stderr, "P51_DIRECT_TOPOLOGY_BEGIN C%zuF%zu %s W30\n",
                 c_count, f_count, topology_profile_name(profile));
    std::fflush(stderr);
    CHECK((c_count == 1 && f_count >= 2 && f_count <= 4) ||
          (f_count == 1 && c_count >= 2 && c_count <= 4));

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(30),
        clock.clock_domain_id, clock.time_namespace_id);
    const size_t link_count = std::max(c_count, f_count);
    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    std::vector<std::unique_ptr<TopologyLink>> links;
    links.reserve(link_count);
    std::map<Id128, size_t> relationship_index;
    std::mutex expected_mutex;
    std::map<std::pair<CStoreGuid, uint64_t>, std::vector<uint8_t>> expected_input;
    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 2U << 20;
    limits.max_live_entries = 256;
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;

    std::vector<std::shared_ptr<P50PreparationAuthority>> authorities(c_count);
    for (size_t c = 0; c != c_count; ++c) {
        const CStoreGuid c_guid{topology_guid(static_cast<uint8_t>(10 + c * 19), false)};
        CHECK(store_identity_guid_valid_for_role(c_guid.bytes,
                                                 kStoreIdentityClientRole));
        authorities[c] = std::make_shared<P50PreparationAuthority>(
            c_guid, caps.zstd, limits, 1, profile);
    }

    for (size_t i = 0; i != link_count; ++i) {
        const size_t c_index = c_count == 1 ? 0 : i;
        const size_t f_index = f_count == 1 ? 0 : i;
        auto link = std::make_unique<TopologyLink>();
        link->c_guid = CStoreGuid{topology_guid(
            static_cast<uint8_t>(10 + c_index * 19), false)};
        link->f_guid = FStoreGuid{topology_guid(
            static_cast<uint8_t>(110 + f_index * 19), true)};
        CHECK(store_identity_guid_valid_for_role(link->f_guid.bytes,
                                                 kStoreIdentityFileRole));
        link->relationship = Id128::from_u64(0x710000 + i);
        link->relationship_epoch = 0x720000 + i;
        link->physical_generation = 0x730000 + i;
        link->c_generation = 0x740000 + c_index;
        link->f_generation = 0x750000 + f_index;
        link->control_generation = 0x760000 + c_index;
        link->control_attempt = 0x770000 + c_index;
        link->authority = authorities[c_index];
        link->armed.reserve(kRefill);
        link->input.reserve(kRefill);
        for (size_t job = 0; job != kRefill; ++job) {
            const uint32_t wire_job = static_cast<uint32_t>(10000 + i * 100 + job);
            auto arm = topology_arm(*link, wire_job, profile);
            link->armed.push_back(topology_armed(*link, std::move(arm), job));
            std::vector<uint8_t> bytes;
            if (profile == ProfileId::ZSTD_TU) {
                bytes.resize(96);
                for (size_t b = 0; b != bytes.size(); ++b)
                    bytes[b] = static_cast<uint8_t>((i * 31 + job * 7 + b) & 0xff);
            } else {
                const std::string source =
                    "#define TOPO_" + std::to_string(i) + "_" +
                    std::to_string(job) + " " + std::to_string(job + 1) +
                    "\nstatic const unsigned int topo_" + std::to_string(i) +
                    "_" + std::to_string(job) + " = TOPO_" +
                    std::to_string(i) + "_" + std::to_string(job) +
                    ";\n/* exact preprocessed source fixture */\n";
                bytes.assign(source.begin(), source.end());
            }
            link->input.push_back(std::move(bytes));
        }
        relationship_index.emplace(link->relationship, i);
        links.push_back(std::move(link));
    }

    asio::io_context f_context;
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::vector<std::unique_ptr<P50ServerEndpoint>> endpoints;
    std::vector<std::future<ServerRunResult>> server_results;
    for (size_t f = 0; f != f_count; ++f) {
        auto acceptor = std::make_unique<tcp::acceptor>(
            f_context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
        P50ServerEndpointConfig server_config;
        server_config.input_job_state = [&](CStoreGuid c_guid, const TxBegin& begin,
                                           const TxCommit& commit,
                                           std::span<const uint8_t> input) {
            std::lock_guard lock(expected_mutex);
            const auto found = expected_input.find({c_guid, begin.tu_seq.value});
            if (found == expected_input.end() || found->second.size() != input.size() ||
                !std::equal(input.begin(), input.end(), found->second.begin()) ||
                begin.raw_bytes != input.size() ||
                begin.raw_digest != icecc::digest128(input) ||
                commit.raw_digest != icecc::digest128(input))
                return InputJobState::Closed;
            return InputJobState::Open;
        };
        server_config.lookup_p51_link_reservation =
            [&, deadline](const LinkHello& hello)
                -> std::optional<P51SourceLinkLease> {
            const auto found = relationship_index.find(hello.relationship_id);
            if (found == relationship_index.end()) return std::nullopt;
            const TopologyLink& link = *links[found->second];
            if (hello.c_store_guid != link.c_guid ||
                hello.f_store_guid != link.f_guid ||
                hello.relationship_epoch != link.relationship_epoch ||
                hello.physical_link_generation != link.physical_generation ||
                hello.window != kWindow || hello.profile != profile)
                return std::nullopt;
            P51SourceLinkLease lease;
            lease.initial_armed = link.armed.front();
            lease.absolute_deadline = deadline;
            lease.relationship_epoch = hello.relationship_epoch;
            lease.history_nonce = hello.history_nonce;
            return lease;
        };
        server_config.consume_p51_job_reservation =
            [&, deadline](const LinkHello& hello, const JobBind& binding)
                -> std::optional<P51SourceJobLease> {
            const auto found = relationship_index.find(hello.relationship_id);
            if (found == relationship_index.end()) return std::nullopt;
            TopologyLink& link = *links[found->second];
            size_t job = link.armed.size();
            for (size_t index = 0; index != link.armed.size(); ++index)
                if (binding.reservation_id ==
                    Id128{link.armed[index].reservation_id}) {
                    job = index;
                    break;
                }
            if (job == link.armed.size() ||
                binding.relationship_ordinal != job + 1 ||
                binding.physical_link_generation != link.physical_generation ||
                binding.profile != profile ||
                binding.raw_bytes != link.input[job].size() ||
                binding.raw_digest != icecc::digest128(link.input[job]) ||
                binding.wire_job_id != link.armed[job].arm.source.wire_job_id ||
                binding.assignment_nonce !=
                    link.armed[job].arm.source.assignment_nonce ||
                binding.source_request_id !=
                    link.armed[job].arm.source.source_request_id)
                return std::nullopt;
            P51SourceJobLease lease;
            lease.armed = link.armed[job];
            lease.absolute_deadline = deadline;
            lease.binding = binding;
            lease.binding_digest = compute_r2_binding_digest(binding);
            lease.input_key = InputRecordKey{link.c_guid, binding.tu_seq};
            {
                std::lock_guard lock(expected_mutex);
                const auto [_, inserted] = expected_input.emplace(
                    std::make_pair(link.c_guid, binding.tu_seq.value),
                    link.input[job]);
                if (!inserted) return std::nullopt;
            }
            return lease;
        };
        server_config.record_p51_job_commit =
            [&](const LinkHello& hello, const JobBind& binding,
                const R2TxCommit& commit) {
            const auto found = relationship_index.find(hello.relationship_id);
            if (found == relationship_index.end()) return false;
            TopologyLink& link = *links[found->second];
            size_t job = link.armed.size();
            for (size_t index = 0; index != link.armed.size(); ++index)
                if (binding.reservation_id ==
                    Id128{link.armed[index].reservation_id}) {
                    job = index;
                    break;
                }
            if (job >= link.input.size() || binding.raw_bytes !=
                    link.input[job].size() ||
                commit.inner.raw_digest != icecc::digest128(link.input[job]))
                return false;
            link.committed.fetch_add(1, std::memory_order_release);
            progress_cv.notify_all();
            return true;
        };
        server_config.acknowledge_p51_receipt =
            [&](const LinkHello& hello, const CommitAck& ack) {
            const auto found = relationship_index.find(hello.relationship_id);
            if (found == relationship_index.end()) return false;
            TopologyLink& link = *links[found->second];
            if (ack.relationship_epoch != link.relationship_epoch ||
                ack.physical_link_generation != link.physical_generation)
                return false;
            link.acknowledged.store(
                static_cast<unsigned>(ack.contiguous_verified_ordinal),
                std::memory_order_release);
            progress_cv.notify_all();
            return true;
        };
        endpoints.push_back(std::make_unique<P50ServerEndpoint>(
            FStoreGuid{topology_guid(static_cast<uint8_t>(110 + f * 19), true)},
            caps, nullptr, nullptr, std::move(server_config)));
        const size_t connections_for_f = c_count == 1 ? 1 : c_count;
        server_results.push_back(asio::co_spawn(
            f_context,
            accept_topology_links(*acceptor, *endpoints.back(), connections_for_f),
            asio::use_future));
        acceptors.push_back(std::move(acceptor));
    }

    std::atomic<bool> hold_readers{true};
    asio::io_context c_context;
    std::vector<std::future<ZstdSourceTransferResult>> results;
    results.reserve(link_count * kRefill);
    for (size_t i = 0; i != link_count; ++i) {
        TopologyLink& link = *links[i];
        ZstdSourceTransferConfig sender_config;
        sender_config.endpoint_caps = caps;
        sender_config.authority_limits = limits;
        sender_config.endpoint_caps.profile = profile;
        sender_config.compression_level = 1;
        sender_config.deadline = deadline.as_steady_time_point();
        sender_config.hold_r2_receipt_reader_for_test = [&] {
            return hold_readers.load(std::memory_order_acquire);
        };
        sender_config.after_r2_bundle_sent_for_test = [&, i](uint64_t) {
            links[i]->sent.fetch_add(1, std::memory_order_release);
            progress_cv.notify_all();
        };
        const PreparationRouteKey route{
            link.f_guid, link.f_generation, profile};
        link.sender = std::make_shared<P50ZstdSourceSender>(
            link.authority, route, PrepareRequestKey{3, 101}, sender_config);
        const tcp::endpoint remote = acceptors[f_count == 1 ? 0 : i]->local_endpoint();
        auto connector = [&, i, remote](auto, auto completion) {
            links[i]->connector_calls.fetch_add(1, std::memory_order_relaxed);
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) { completion(-1); return; }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(remote.port());
            const auto ip = remote.address().to_v4().to_bytes();
            std::memcpy(&address.sin_addr, ip.data(), ip.size());
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) != 0) {
                (void)::close(fd);
                completion(-1);
                return;
            }
            completion(fd);
        };
        for (size_t job = 0; job != kRefill; ++job) {
            const PrepareRequestKey request{
                3, link.armed[job].arm.source.source_request_id};
            results.push_back(asio::co_spawn(
                c_context,
                link.sender->transfer_p51_route(
                    link.armed[job], link.physical_generation, connector,
                    request, deadline.as_steady_time_point(), link.input[job]),
                asio::use_future));
        }
    }

    std::thread f_thread([&] { f_context.run(); });
    std::thread c_thread([&] { c_context.run(); });
    struct Cleanup {
        std::atomic<bool>& hold;
        asio::io_context& c;
        asio::io_context& f;
        std::thread& c_thread;
        std::thread& f_thread;
        ~Cleanup() {
            hold.store(false, std::memory_order_release);
            c.stop();
            f.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    } cleanup{hold_readers, c_context, f_context, c_thread, f_thread};

    auto every_link_at_window = [&] {
        for (const auto& link : links)
            if (link->sent.load(std::memory_order_acquire) < kWindow ||
                link->committed.load(std::memory_order_acquire) < kWindow)
                return false;
        return true;
    };
    auto server_failed_early = [&] {
        return std::any_of(server_results.begin(), server_results.end(),
            [](std::future<ServerRunResult>& result) {
                return result.wait_for(std::chrono::milliseconds(0)) ==
                       std::future_status::ready;
            });
    };
    {
        std::unique_lock lock(progress_mutex);
        const bool reached = progress_cv.wait_for(
            lock, std::chrono::seconds(20), [&] {
                return every_link_at_window() || server_failed_early();
            });
        const bool window_reached = every_link_at_window();
        if (!window_reached) {
            for (size_t i = 0; i != links.size(); ++i)
                std::fprintf(stderr,
                    "TOPOLOGY_LINK i=%zu Cguid=%02x Fguid=%02x sent=%u committed=%u ack=%u connects=%u\n",
                    i, links[i]->c_guid.bytes[0], links[i]->f_guid.bytes[0],
                    links[i]->sent.load(), links[i]->committed.load(),
                    links[i]->acknowledged.load(), links[i]->connector_calls.load());
            lock.unlock();
            for (size_t i = 0; i != server_results.size(); ++i) {
                if (server_results[i].wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::ready) {
                    const ServerRunResult diagnostic = server_results[i].get();
                    std::fprintf(stderr,
                        "TOPOLOGY_F_SERVER i=%zu status=%u error=%u detail=%.*s\n",
                        i, static_cast<unsigned>(diagnostic.status),
                        diagnostic.terminal_error ? diagnostic.terminal_error->code : 0,
                        diagnostic.terminal_error
                            ? static_cast<int>(diagnostic.terminal_error->detail.size()) : 0,
                        diagnostic.terminal_error
                            ? diagnostic.terminal_error->detail.data() : "");
                }
            }
            for (size_t i = 0; i != results.size(); ++i) {
                if (results[i].wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::ready) {
                    const ZstdSourceTransferResult diagnostic = results[i].get();
                    std::fprintf(stderr,
                        "TOPOLOGY_C_RESULT i=%zu status=%u error=%u detail=%.*s\n",
                        i, static_cast<unsigned>(diagnostic.status),
                        diagnostic.terminal_error ? diagnostic.terminal_error->code : 0,
                        diagnostic.terminal_error
                            ? static_cast<int>(diagnostic.terminal_error->detail.size()) : 0,
                        diagnostic.terminal_error
                            ? diagnostic.terminal_error->detail.data() : "");
                }
            }
            std::fflush(stderr);
        }
        CHECK(reached && window_reached);
    }
    unsigned admitted_before_ack = 0;
    uint64_t raw_bytes_before_ack = 0;
    for (const auto& link : links) {
        CHECK(link->sent.load(std::memory_order_acquire) >= kWindow);
        CHECK(link->committed.load(std::memory_order_acquire) >= kWindow);
        CHECK(link->acknowledged.load(std::memory_order_acquire) == 0);
        admitted_before_ack += link->sent.load(std::memory_order_acquire);
        for (size_t job = 0; job != kWindow; ++job)
            raw_bytes_before_ack += link->input[job].size();
    }
    CHECK(admitted_before_ack == link_count * kWindow);

    hold_readers.store(false, std::memory_order_release);
    progress_cv.notify_all();
    for (auto& result : results)
        CHECK(result.get().status == ZstdSourceTransferStatus::Committed);
    auto all_acknowledged = [&] {
        return std::all_of(links.begin(), links.end(), [](const auto& link) {
            return link->acknowledged.load(std::memory_order_acquire) == kRefill;
        });
    };
    {
        std::unique_lock lock(progress_mutex);
        CHECK(progress_cv.wait_for(lock, std::chrono::seconds(5), all_acknowledged));
    }
    unsigned aggregate_committed = 0;
    unsigned aggregate_acknowledged = 0;
    for (const auto& link : links) {
        CHECK(link->sent.load(std::memory_order_acquire) == kRefill);
        CHECK(link->committed.load(std::memory_order_acquire) == kRefill);
        CHECK(link->acknowledged.load(std::memory_order_acquire) == kRefill);
        CHECK(link->connector_calls.load(std::memory_order_relaxed) == 1);
        aggregate_committed += link->committed.load(std::memory_order_acquire);
        aggregate_acknowledged +=
            link->acknowledged.load(std::memory_order_acquire);
        link->sender->retire_for_replacement();
    }
    CHECK(aggregate_committed == link_count * kRefill);
    CHECK(aggregate_acknowledged == link_count * kRefill);
    for (auto& result : server_results)
        CHECK(result.get().status == ServerRunStatus::Disconnected);
    std::fprintf(stderr, "P51_DIRECT_TOPOLOGY_PASS C%zuF%zu %s links=%zu peak=%u raw_bytes=%llu committed=%u ack=%u\n",
                 c_count, f_count, topology_profile_name(profile), link_count, admitted_before_ack,
                 static_cast<unsigned long long>(raw_bytes_before_ack),
                 aggregate_committed, aggregate_acknowledged);
    std::fflush(stderr);
}

void test_retired_route_reaped_when_background_reader_goes_idle() {
    const ProfileId profile = ProfileId::ZSTD_TU;
    const CStoreGuid c_guid{topology_guid(31, false)};
    const FStoreGuid f_guid{topology_guid(131, true)};
    const P50RouteRelationship route{c_guid, f_guid, 91, profile};
    const std::vector<uint8_t> source{'i', 'd', 'l', 'e', '-', 'r', 'e', 'a', 'p'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(8),
        clock.clock_domain_id, clock.time_namespace_id);

    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    std::atomic<bool> hold_reader{true};
    std::atomic<bool> reader_entered{false};
    std::atomic<unsigned> pump_notifications{0};
    std::atomic<unsigned> reap_notifications{0};
    asio::io_context context;
    P50CRouteOwner* owner_ptr = nullptr;
    P50RouteOwnerConfig owner_config = config(profile);
    owner_config.hold_r2_receipt_reader_for_test = [&] {
        reader_entered.store(true, std::memory_order_release);
        return hold_reader.load(std::memory_order_acquire);
    };
    owner_config.post_retired_reap = [&] {
        pump_notifications.fetch_add(1, std::memory_order_release);
        context.post([&] {
            if (owner_ptr)
                owner_ptr->reap_retired_p51();
        });
    };
    owner_config.after_retired_route_reaped_for_test = [&] {
        reap_notifications.fetch_add(1, std::memory_order_release);
    };
    P50CRouteOwner owner(std::move(owner_config));
    owner_ptr = &owner;

    TopologyLink link;
    link.c_guid = c_guid;
    link.f_guid = f_guid;
    link.relationship = Id128::from_u64(0x9131);
    link.relationship_epoch = 1;
    link.physical_generation = 17;
    link.c_generation = 18;
    link.f_generation = route.f_store_generation;
    link.control_generation = 19;
    link.control_attempt = 20;
    auto arm = topology_arm(link, 231, profile);
    const PrepareRequestKey request_key{
        arm.source.assignment_epoch, arm.source.source_request_id};
    P51SourceArmedFields armed = topology_armed(link, std::move(arm), 1);
    armed.selected_window = 1;
    CHECK(armed.valid());

    std::atomic<unsigned> commit_calls{0};
    P50ServerEndpointConfig server_config;
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.relationship_id != link.relationship ||
            hello.f_store_guid != f_guid ||
            hello.f_store_generation != route.f_store_generation)
            return std::nullopt;
        return P51SourceLinkLease{armed, deadline, false,
                                  hello.relationship_epoch,
                                  hello.history_nonce, 0, 0};
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (hello.relationship_id != link.relationship ||
            binding.reservation_id != Id128{armed.reservation_id} ||
            binding.raw_bytes != source.size() ||
            binding.raw_digest != icecc::digest128(source))
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = armed;
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello&, const JobBind&, const R2TxCommit& commit) {
            if (commit.inner.raw_digest != icecc::digest128(source))
                return false;
            commit_calls.fetch_add(1, std::memory_order_release);
            return true;
        };
    server_config.acknowledge_p51_receipt =
        [](const LinkHello&, const CommitAck&) { return true; };
    P50ServerEndpoint server(f_guid, caps, nullptr, nullptr,
                             std::move(server_config));
    asio::ip::tcp::acceptor acceptor(
        context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
    auto accept_r2_one = [&]() -> asio::awaitable<ServerRunResult> {
        tcp::socket socket(co_await asio::this_coro::executor);
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        co_return co_await server.run_adopted_r2(std::move(socket));
    };
    std::promise<ServerRunResult> server_promise;
    auto server_future = server_promise.get_future();
    bool server_done = false;
    bool transfer_done = false;
    bool watchdog_fired = false;
    bool active_new_relationship_rejected = false;
    asio::steady_timer fixture_watchdog(context);
    fixture_watchdog.expires_after(std::chrono::seconds(10));
    fixture_watchdog.async_wait([&](const boost::system::error_code& error) {
        if (error)
            return;
        watchdog_fired = true;
        hold_reader.store(false, std::memory_order_release);
        boost::system::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        (void)owner.retire_f_store_exact_p51(f_guid, route.f_store_generation);
    });
    auto cancel_fixture_watchdog_if_done = [&] {
        if (server_done && transfer_done)
            fixture_watchdog.cancel();
    };
    asio::co_spawn(
        context, accept_r2_one(),
        [&](std::exception_ptr error, ServerRunResult result) {
            server_done = true;
            if (error)
                server_promise.set_exception(error);
            else
                server_promise.set_value(std::move(result));
            cancel_fixture_watchdog_if_done();
        });

    const auto remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector = [remote](auto, auto completion) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { completion(-1); return; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(remote.port());
        const auto ip = remote.address().to_v4().to_bytes();
        std::memcpy(&address.sin_addr, ip.data(), ip.size());
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0) {
            (void)::close(fd);
            completion(-1);
            return;
        }
        completion(fd);
    };
    std::promise<ZstdSourceTransferResult> transfer_promise;
    auto transfer_future = transfer_promise.get_future();
    bool retired = false;
    bool caller_finished_before_reap = false;
    asio::co_spawn(
        context,
        owner.transfer_p51(route, armed, std::move(connector),
                           request_key,
                           deadline.as_steady_time_point(), source),
        [&](std::exception_ptr error, ZstdSourceTransferResult result) {
            transfer_done = true;
            caller_finished_before_reap = retired &&
                hold_reader.load(std::memory_order_acquire) &&
                reap_notifications.load(std::memory_order_acquire) == 0;
            if (error)
                transfer_promise.set_exception(error);
            else
                transfer_promise.set_value(std::move(result));
            hold_reader.store(false, std::memory_order_release);
            cancel_fixture_watchdog_if_done();
        });
    asio::steady_timer retirement_watch(context);
    const auto reader_wait_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
    // Keep the coroutine closure alive through context.run(). The reader
    // remains held after retirement until the original caller has returned,
    // so caller-return cleanup cannot satisfy the post-pump reap assertion.
    auto watch_retirement = [&, reader_wait_deadline]() -> asio::awaitable<void> {
        while (!reader_entered.load(std::memory_order_acquire) ||
               commit_calls.load(std::memory_order_acquire) != 1) {
            if (std::chrono::steady_clock::now() >= reader_wait_deadline)
                break;
            retirement_watch.expires_after(std::chrono::milliseconds(1));
            co_await retirement_watch.async_wait(asio::use_awaitable);
        }
        if (reader_entered.load(std::memory_order_acquire) &&
            commit_calls.load(std::memory_order_acquire) == 1) {
            // A higher-epoch, different-ID ARM cannot retire this route while
            // its original TU is still awaiting its exact receipt.
            P51SourceArmedFields newer_arm = armed;
            newer_arm.logical_relationship_id =
                Id128::from_u64(0x91310001).bytes;
            ++newer_arm.relationship_epoch;
            CHECK(newer_arm.valid());
            const auto active_result = co_await owner.transfer_p51(
                route, newer_arm, connector, request_key,
                deadline.as_steady_time_point(), source);
            active_new_relationship_rejected =
                active_result.status == ZstdSourceTransferStatus::InvalidRequest &&
                !active_result.replacement_required;
            retired = owner.retire_f_store_exact_p51(
                f_guid, route.f_store_generation);
        }
        if (!retired)
            hold_reader.store(false, std::memory_order_release);
        co_return;
    };
    asio::co_spawn(context, watch_retirement(), asio::detached);

    context.run();
    const ZstdSourceTransferResult transfer_result = transfer_future.get();
    ServerRunResult server_result;
    try {
        server_result = server_future.get();
    } catch (...) {
        CHECK(watchdog_fired);
    }
    CHECK(!watchdog_fired);
    CHECK(active_new_relationship_rejected);
    CHECK(retired);
    CHECK(caller_finished_before_reap);
    CHECK(transfer_result.status != ZstdSourceTransferStatus::Committed);
    CHECK(commit_calls.load(std::memory_order_acquire) == 1);
    CHECK(pump_notifications.load(std::memory_order_acquire) != 0);
    CHECK(reap_notifications.load(std::memory_order_acquire) == 1);
    CHECK(server_result.status == ServerRunStatus::Disconnected ||
          server_result.status == ServerRunStatus::TerminalError);
    std::puts("P51_ROUTE_OWNER idle background-pump retirement reap: ok");
}

void test_concurrent_same_successor_joins_one_rebound_sender() {
    const ProfileId profile = ProfileId::ZSTD_TU;
    const CStoreGuid c_guid{topology_guid(41, false)};
    const FStoreGuid f_guid{topology_guid(141, true)};
    const P50RouteRelationship route{c_guid, f_guid, 92, profile};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(18),
        clock.clock_domain_id, clock.time_namespace_id);

    TopologyLink old_link;
    old_link.c_guid = c_guid;
    old_link.f_guid = f_guid;
    old_link.relationship = Id128::from_u64(0x9141);
    old_link.relationship_epoch = 1;
    old_link.c_generation = 42;
    old_link.f_generation = route.f_store_generation;
    old_link.control_generation = 43;
    old_link.control_attempt = 44;
    auto old_arm = topology_arm(old_link, 241, profile);
    P51SourceArmedFields old_armed = topology_armed(
        old_link, std::move(old_arm), 241);
    old_armed.selected_window = 1;
    const PrepareRequestKey old_request{
        old_armed.arm.source.assignment_epoch,
        old_armed.arm.source.source_request_id};
    const std::vector<uint8_t> old_source{'o','l','d','-','r','e','l','a','t','i','o','n'};

    TopologyLink successor_link;
    successor_link.c_guid = old_link.c_guid;
    successor_link.f_guid = old_link.f_guid;
    successor_link.relationship = Id128::from_u64(0x9142);
    successor_link.relationship_epoch = 2;
    successor_link.c_generation = old_link.c_generation;
    successor_link.f_generation = old_link.f_generation;
    successor_link.control_generation = old_link.control_generation;
    successor_link.control_attempt = old_link.control_attempt;
    std::array<P51SourceArmedFields, 2> successor_armed;
    std::array<std::vector<uint8_t>, 2> successor_source{
        std::vector<uint8_t>{'n','e','w','-','o','n','e'},
        std::vector<uint8_t>{'n','e','w','-','t','w','o'}};
    for (size_t i = 0; i != successor_armed.size(); ++i) {
        auto arm = topology_arm(successor_link,
                                static_cast<uint32_t>(242 + i), profile);
        successor_armed[i] = topology_armed(
            successor_link, std::move(arm), 242 + i);
        successor_armed[i].selected_window = 2;
        CHECK(successor_armed[i].valid());
    }
    CHECK(old_armed.valid());

    std::atomic<bool> hold_ack_pump{true};
    std::atomic<bool> ack_pump_entered{false};
    std::atomic<unsigned> rebind_waiters{0};
    std::atomic<unsigned> retired_reaps{0};
    std::atomic<unsigned> connectors{0};
    std::atomic<unsigned> old_commits{0};
    std::array<std::atomic<unsigned>, 2> successor_commits{};
    std::array<std::atomic<unsigned>, 2> successor_acks{};
    std::atomic<unsigned> input_mismatches{0};
    std::mutex observer_mutex;
    std::condition_variable observer_cv;

    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50RouteOwnerConfig owner_config = config(profile);
    owner_config.authority_limits.max_speculative_tus = 2;
    owner_config.authority_limits.max_speculative_raw_bytes = 1U << 20;
    owner_config.authority_limits.max_live_entries = 16;
    owner_config.max_relationships = 4;
    owner_config.maximum_duration = std::chrono::seconds(18);
    owner_config.hold_r2_ack_pump_for_test = [&] {
        ack_pump_entered.store(true, std::memory_order_release);
        observer_cv.notify_all();
        return hold_ack_pump.load(std::memory_order_acquire);
    };
    owner_config.after_r2_rebind_wait_for_test = [&] {
        rebind_waiters.fetch_add(1, std::memory_order_release);
        observer_cv.notify_all();
    };

    asio::io_context context;
    P50CRouteOwner* owner_ptr = nullptr;
    owner_config.post_retired_reap = [&] {
        context.post([&] {
            if (owner_ptr)
                owner_ptr->reap_retired_p51();
        });
    };
    owner_config.after_retired_route_reaped_for_test = [&] {
        retired_reaps.fetch_add(1, std::memory_order_release);
        observer_cv.notify_all();
    };
    P50CRouteOwner owner(std::move(owner_config));
    owner_ptr = &owner;

    P50ServerEndpointConfig server_config;
    const auto find_job = [&](const Id128& reservation) -> int {
        if (reservation == Id128{old_armed.reservation_id}) return -1;
        for (size_t i = 0; i != successor_armed.size(); ++i)
            if (reservation == Id128{successor_armed[i].reservation_id})
                return static_cast<int>(i);
        return -2;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        const int job = find_job(hello.reservation_id);
        if ((hello.relationship_id != old_link.relationship &&
             hello.relationship_id != successor_link.relationship) ||
            hello.f_store_guid != f_guid ||
            hello.f_store_generation != route.f_store_generation ||
            hello.c_store_guid != c_guid || job == -2 ||
            (hello.relationship_id == old_link.relationship && job != -1) ||
            (hello.relationship_id == successor_link.relationship && job == -1))
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = job == -1 ? old_armed
                                        : successor_armed[static_cast<size_t>(job)];
        lease.absolute_deadline = deadline;
        lease.relationship_epoch = hello.relationship_epoch;
        lease.history_nonce = hello.history_nonce;
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        const int job = find_job(binding.reservation_id);
        const bool old = job == -1;
        const Id128 expected_relationship = old
            ? old_link.relationship : successor_link.relationship;
        const std::span<const uint8_t> expected_source = old
            ? std::span<const uint8_t>(old_source)
            : job >= 0 ? std::span<const uint8_t>(
                             successor_source[static_cast<size_t>(job)])
                       : std::span<const uint8_t>();
        if (job == -2 || hello.relationship_id != expected_relationship ||
            binding.profile != profile || binding.raw_bytes != expected_source.size() ||
            binding.raw_digest != icecc::digest128(expected_source))
            return std::nullopt;
        const P51SourceArmedFields& armed = old
            ? old_armed : successor_armed[static_cast<size_t>(job)];
        if (binding.reservation_id != Id128{armed.reservation_id} ||
            binding.wire_job_id != armed.arm.source.wire_job_id ||
            binding.source_request_id != armed.arm.source.source_request_id ||
            binding.assignment_nonce != armed.arm.source.assignment_nonce ||
            binding.assignment_epoch != armed.arm.source.assignment_epoch)
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = armed;
        lease.armed.relationship_epoch = hello.relationship_epoch;
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.input_job_state =
        [&](CStoreGuid, const TxBegin&, const TxCommit& commit,
            std::span<const uint8_t> bytes) {
        bool exact = commit.raw_digest == icecc::digest128(old_source) &&
                     bytes.size() == old_source.size() &&
                     std::equal(bytes.begin(), bytes.end(), old_source.begin());
        for (const auto& source : successor_source)
            exact = exact || (commit.raw_digest == icecc::digest128(source) &&
                bytes.size() == source.size() &&
                std::equal(bytes.begin(), bytes.end(), source.begin()));
        if (!exact)
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        return InputJobState::Open;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding,
            const R2TxCommit& commit) {
        const int job = find_job(binding.reservation_id);
        if (commit.inner.raw_digest != binding.raw_digest || job == -2)
            return false;
        if (job == -1)
            old_commits.fetch_add(1, std::memory_order_release);
        else if (hello.relationship_id == successor_link.relationship)
            successor_commits[static_cast<size_t>(job)].fetch_add(
                1, std::memory_order_release);
        else
            return false;
        observer_cv.notify_all();
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        if (ack.relationship_id != hello.relationship_id ||
            ack.relationship_epoch != hello.relationship_epoch ||
            ack.physical_link_generation != hello.physical_link_generation)
            return false;
        if (hello.relationship_id == successor_link.relationship) {
            if (ack.contiguous_verified_ordinal == 0 ||
                ack.contiguous_verified_ordinal > successor_acks.size())
                return false;
            successor_acks[static_cast<size_t>(ack.contiguous_verified_ordinal - 1)]
                .store(1, std::memory_order_release);
        }
        observer_cv.notify_all();
        return true;
    };
    P50ServerEndpoint server(f_guid, caps, nullptr, nullptr,
                             std::move(server_config));
    tcp::acceptor acceptor(
        context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
    std::atomic<bool> old_history_retired{false};
    auto serve_old_then_successor = [&]() -> asio::awaitable<ServerRunResult> {
        tcp::socket old_socket(co_await asio::this_coro::executor);
        co_await acceptor.async_accept(old_socket, asio::use_awaitable);
        const ServerRunResult old_result = co_await server.run_adopted_r2(
            std::move(old_socket));
        if (!server.retire_idle_p51_route_history(
                c_guid, profile, old_link.relationship,
                old_link.relationship_epoch))
            throw std::runtime_error("old idle F relationship history did not retire");
        old_history_retired.store(true, std::memory_order_release);
        tcp::socket successor_socket(co_await asio::this_coro::executor);
        co_await acceptor.async_accept(successor_socket, asio::use_awaitable);
        const ServerRunResult successor_result = co_await server.run_adopted_r2(
            std::move(successor_socket));
        if (old_result.status != ServerRunStatus::Disconnected)
            co_return old_result;
        co_return successor_result;
    };
    std::promise<ServerRunResult> server_promise;
    auto server_future = server_promise.get_future();
    bool server_done = false;
    asio::steady_timer watchdog(context);
    watchdog.expires_after(std::chrono::seconds(22));
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (error) return;
        hold_ack_pump.store(false, std::memory_order_release);
        if (owner_ptr)
            (void)owner_ptr->retire_f_store_exact_p51(
                f_guid, route.f_store_generation);
        boost::system::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
    });
    asio::co_spawn(context, serve_old_then_successor(),
        [&](std::exception_ptr error, ServerRunResult result) {
            server_done = true;
            if (error) server_promise.set_exception(error);
            else server_promise.set_value(std::move(result));
            watchdog.cancel();
        });

    const tcp::endpoint remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector = [&, remote](auto, auto completion) {
        connectors.fetch_add(1, std::memory_order_relaxed);
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { completion(-1); return; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(remote.port());
        const auto ip = remote.address().to_v4().to_bytes();
        std::memcpy(&address.sin_addr, ip.data(), ip.size());
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0) {
            (void)::close(fd);
            completion(-1);
            return;
        }
        completion(fd);
    };

    auto work = asio::make_work_guard(context);
    std::thread event_thread([&] { context.run(); });
    struct Cleanup {
        asio::io_context& context;
        asio::executor_work_guard<asio::io_context::executor_type>& work;
        std::thread& thread;
        std::atomic<bool>& release;
        ~Cleanup() {
            release.store(false, std::memory_order_release);
            context.stop();
            work.reset();
            if (thread.joinable()) thread.join();
        }
    } cleanup{context, work, event_thread, hold_ack_pump};

    auto old_transfer_future = asio::co_spawn(
        context, owner.transfer_p51(
            route, old_armed, connector, old_request,
            deadline.as_steady_time_point(), old_source),
        asio::use_future);
    CHECK(old_transfer_future.wait_until(deadline.as_steady_time_point()) ==
          std::future_status::ready);
    const auto old_result = old_transfer_future.get();
    CHECK(old_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(old_result.raw_digest == icecc::digest128(old_source));
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_cv.wait_until(lock, deadline.as_steady_time_point(), [&] {
            return ack_pump_entered.load(std::memory_order_acquire);
        }));
    }
    CHECK(old_commits.load(std::memory_order_acquire) == 1);

    std::array<std::future<ZstdSourceTransferResult>, 2> successor_results;
    for (size_t i = 0; i != successor_results.size(); ++i) {
        const PrepareRequestKey request{
            successor_armed[i].arm.source.assignment_epoch,
            successor_armed[i].arm.source.source_request_id};
        successor_results[i] = asio::co_spawn(
            context, owner.transfer_p51(
                route, successor_armed[i], connector, request,
                deadline.as_steady_time_point(), successor_source[i]),
            asio::use_future);
    }
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_cv.wait_until(lock, deadline.as_steady_time_point(), [&] {
            return rebind_waiters.load(std::memory_order_acquire) >= 2;
        }));
    }
    CHECK(!old_history_retired.load(std::memory_order_acquire));
    hold_ack_pump.store(false, std::memory_order_release);
    observer_cv.notify_all();

    for (size_t i = 0; i != successor_results.size(); ++i) {
        CHECK(successor_results[i].wait_until(deadline.as_steady_time_point()) ==
              std::future_status::ready);
        const auto result = successor_results[i].get();
        CHECK(result.status == ZstdSourceTransferStatus::Committed);
        CHECK(result.raw_bytes == successor_source[i].size());
        CHECK(result.raw_digest == icecc::digest128(successor_source[i]));
    }
    CHECK(old_history_retired.load(std::memory_order_acquire));
    CHECK(input_mismatches.load(std::memory_order_acquire) == 0);
    CHECK(old_commits.load(std::memory_order_acquire) == 1);
    for (const auto& count : successor_commits)
        CHECK(count.load(std::memory_order_acquire) == 1);
    CHECK(connectors.load(std::memory_order_acquire) == 2);
    {
        std::unique_lock lock(observer_mutex);
        CHECK(observer_cv.wait_until(lock, deadline.as_steady_time_point(), [&] {
            return successor_acks[1].load(std::memory_order_acquire) == 1;
        }));
    }
    CHECK(successor_acks[1].load(std::memory_order_acquire) == 1);
    CHECK(retired_reaps.load(std::memory_order_acquire) == 1);

    std::promise<bool> retire_promise;
    auto retire_future = retire_promise.get_future();
    asio::post(context, [&] {
        retire_promise.set_value(owner.retire_f_store_exact_p51(
            f_guid, route.f_store_generation));
    });
    CHECK(retire_future.wait_for(std::chrono::seconds(3)) ==
          std::future_status::ready);
    CHECK(retire_future.get());
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    const auto server_result = server_future.get();
    CHECK(server_result.status == ServerRunStatus::Disconnected);
    work.reset();
    context.stop();
    event_thread.join();
    CHECK(server_done);
    std::puts("P51_ROUTE_OWNER concurrent same-successor callers share one rebound link: ok");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::strcmp(argv[1], "--replacement-trigger") == 0) {
        test_relationship_table_cap_requests_replacement();
        test_p51_route_owner_preserves_precise_capacity_trigger();
        return 0;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--same-successor-concurrency") == 0) {
        test_concurrent_same_successor_joins_one_rebound_sender();
        return 0;
    }
    test_source_transfer_operation_wire();
    test_long_lived_relationship_owner();
    test_same_request_route_fork_wire();
    test_multiroute_release_lifetime();
    test_tu_seq_reservation_and_exhaustion();
    test_relationship_validation();
    test_p29v1_retry_and_reset_owner();
    test_p29v1_relationship_owner();
    test_relationship_table_cap_requests_replacement();
    test_p51_route_owner_preserves_precise_capacity_trigger();
    test_aborted_route_block_is_defined_on_other_f();
    test_transport_loss_does_not_reject_another_worker();
    test_typed_poison_catch_is_owner_wide();
    test_typed_poison_does_not_destroy_another_active_route();
    test_interner_fault_is_sticky_only_for_p29v1();
    for (const ProfileId profile :
         {ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE}) {
        test_p51_w30_direct_topology(1, 2, profile);
        test_p51_w30_direct_topology(1, 3, profile);
        test_p51_w30_direct_topology(1, 4, profile);
        test_p51_w30_direct_topology(2, 1, profile);
        test_p51_w30_direct_topology(3, 1, profile);
        test_p51_w30_direct_topology(4, 1, profile);
    }
    test_retired_route_reaped_when_background_reader_goes_idle();
    test_concurrent_same_successor_joins_one_rebound_sender();
}
