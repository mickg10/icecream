#include "../cache/p50_source_ingress.h"

#include <sys/wait.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace icecc::p50;
using namespace icecc::p50::daemon;

namespace {
using Clock = SourceIngress::Clock;
using TimePoint = SourceIngress::TimePoint;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

P50SourceArmFields arm() {
    P50SourceArmFields a;
    a.wire_job_id = 1; a.assignment_epoch = 2; a.assignment_nonce = 3;
    a.selected_f_host = "selected-f"; a.selected_f_ordinary_port = 4100;
    a.selected_f_cache_port = 4200; a.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    a.cache_profile = CACHE_PROFILE_ZSTD_TU; a.logical_job = 4;
    a.compiler_attempt = 5; a.c_store_generation = 6;
    a.c_store_derivation_version = kStoreIdentityDerivationVersion;
    for (size_t i = 0; i != a.c_store_guid.size(); ++i)
        a.c_store_guid[i] = static_cast<uint8_t>(0x40 + i);
    a.c_store_guid[kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~kStoreIdentityRoleMask);
    a.source_request_id = 17; a.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    a.c_control_generation = 7; a.c_control_attempt = 8;
    return a;
}

P50SourceArmedMsg ack(const P50SourceArmFields& a, uint32_t budget = 2500) {
    std::array<uint8_t, 16> f{};
    for (size_t i = 0; i != f.size(); ++i) f[i] = static_cast<uint8_t>(0xd0 + i);
    f[kStoreIdentityRoleByte] |= kStoreIdentityFileRole;
    return P50SourceArmedMsg(a, 42, 43, 45, f,
                             kStoreIdentityDerivationVersion, 44, budget);
}

struct Fixture {
    explicit Fixture(uint32_t budget = 2500)
        : owner(1024), request(arm()), acknowledged(ack(request.arm, budget)),
          now(Clock::time_point{} + std::chrono::seconds(10)), pid(4242) {}

    void allocate() {
        auto value = owner.allocate(request, pid, now);
        require(value.has_value(), "allocation failed"); lease = *value;
        ingress = owner.activate(lease); require(ingress != nullptr, "activation failed");
    }
    void refresh() { lease = ingress->lease(); }
    void arm_f() {
        require(ingress->accept_f_source_armed(lease, acknowledged, now) ==
                    SourceIngressResult::Accepted, "F ACK rejected"); refresh();
    }
    void source() {
        const std::array<uint8_t, 5> bytes{'c', 'p', 'p', '\n', 0};
        require(ingress->append_wrapper_bytes(lease, bytes, now) ==
                    SourceIngressResult::Accepted, "source append failed");
        require(ingress->observe_wrapper_eof(lease, now) ==
                    SourceIngressResult::Accepted, "EOF failed");
    }
    void wait_ok() {
        require(ingress->observe_cpp_waitpid(lease, pid, 0, now) ==
                    SourceIngressResult::Accepted, "waitpid failed");
    }
    void finalize() {
        auto value = ingress->take_source_finalize(lease, now);
        if (!value.has_value() || !value->valid())
            throw std::runtime_error("finalize capability missing state=" +
                                     std::to_string(static_cast<int>(ingress->state())) +
                                     " staged=" + std::to_string(ingress->lease().lease_serial));
        finalize_cap = std::move(*value); refresh();
    }
    void ready() {
        arm_f(); source(); wait_ok(); finalize();
    }

    SourceIngressOwner owner;
    P50SourceArmMsg request;
    P50SourceArmedMsg acknowledged;
    TimePoint now;
    pid_t pid;
    SourceIngressLease lease{};
    SourceIngress* ingress = nullptr;
    SourceFinalize finalize_cap;
};

InputRecordKey key(const Fixture& f) {
    InputRecordKey value;
    value.c_store_guid.bytes = f.request.arm.c_store_guid;
    value.tu_seq.value = 99;
    return value;
}

CacheWireSettlementWitness witness(const Fixture& f, const CacheWireStart& start,
                                   CacheWireSettlementDisposition disposition) {
    CacheWireSettlementWitness value;
    value.disposition = disposition; value.lease = f.ingress->lease();
    value.cache_session_attempt = start.cache_session_attempt;
    value.operation_id = start.operation_id; value.input_key = start.input_key;
    value.raw_bytes.assign(start.raw_bytes->begin(), start.raw_bytes->end());
    value.raw_digest = start.raw_digest;
    if (disposition == CacheWireSettlementDisposition::CommittedInput)
        value.committed_input = start.input_key;
    return value;
}

void two_latches_and_order() {
    Fixture f; f.allocate(); f.source(); f.wait_ok();
    f.finalize();
    require(!f.ingress->start_cache_wire(f.lease, key(f), 1, 17, f.now),
                           "CacheWire started without F ACK");
    f.arm_f();
    require(!f.ingress->start_cache_wire(f.lease, key(f), 1, 17, f.now),
            "CacheWire started with unconsumed finalize capability");
    require(f.finalize_cap.consume(), "finalize capability did not consume");
    require(f.ingress->accept_f_source_armed(f.lease, f.acknowledged, f.now) ==
                SourceIngressResult::Duplicate,
            "duplicate F ACK was accepted as a new arm");
    auto start = f.ingress->start_cache_wire(f.lease, key(f), 1, 17, f.now);
    require(start.has_value(), "exact conjunction did not start CacheWire");
    require(f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxBody, f.now) ==
                SourceIngressResult::InvalidOrder, "BODY before BEGIN accepted");
}

void duplicate_cachewire_frame_reconciles() {
    Fixture f; f.allocate(); f.ready();
    require(f.finalize_cap.consume(), "duplicate-frame capability did not consume");
    auto start = f.ingress->start_cache_wire(f.lease, key(f), 3, 19, f.now);
    require(start.has_value(), "duplicate-frame CacheWire did not start");
    require(f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxBegin, f.now) ==
                SourceIngressResult::Accepted &&
            f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxBegin, f.now) ==
                SourceIngressResult::Duplicate &&
            f.ingress->cache_wire_state() == CacheWireState::Reconcile,
            "duplicate CacheWire frame did not enter reconcile");
    auto pre = witness(f, *start, CacheWireSettlementDisposition::ProvedPreDurable);
    require(f.ingress->settle_cache_wire(f.lease, pre, f.now) ==
                SourceIngressResult::Accepted &&
            f.owner.cleanup(f.lease) == SourceIngressResult::Accepted,
            "duplicate-frame reconcile did not settle safely");
}

void eof_waitpid_and_owner_fences() {
    Fixture f; f.allocate(); f.source();
    require(!f.ingress->take_source_finalize(f.lease, f.now), "EOF finalized source");
    require(f.ingress->observe_cpp_waitpid(f.lease, f.pid + 1, 0, f.now) ==
                SourceIngressResult::WrongPid, "wrong PID crossed fence");
    SourceIngressLease serial = f.lease; ++serial.lease_serial;
    require(f.ingress->observe_wrapper_eof(serial, f.now) ==
                SourceIngressResult::WrongOwner, "serial crossed owner fence");
    f.wait_ok(); f.finalize();
    require(f.finalize_cap.consume(), "finalize capability did not consume");
    require(!f.finalize_cap.consume(), "finalize capability consumed twice");
}

void deadline_is_nonrenewable() {
    Fixture f(5); f.allocate(); auto allocation_deadline = f.ingress->deadline();
    f.arm_f(); require(f.ingress->deadline() < allocation_deadline,
                       "ACK renewed deadline");
    auto deadline = f.ingress->deadline();
    require(f.owner.sweep(f.lease, deadline - std::chrono::milliseconds(1)) ==
                SourceIngressResult::NotReady,
            "deadline sweep fired early");
    require(f.owner.sweep(f.lease, deadline) == SourceIngressResult::DeadlineExpired,
            "silent sweep did not expire");
    require(f.ingress->state() == SourceIngressState::Expired, "wrong expiry state");
}

void root_alias_rejected() {
    auto a = arm(); auto bad = ack(a);
    bad.f_store_guid = a.c_store_guid;
    bad.f_store_guid[kStoreIdentityRoleByte] |= kStoreIdentityFileRole;
    require(!bad.valid_payload(), "role-bit-only C/F root alias accepted");
    bad = ack(a);
    bad.f_store_guid.fill(0);
    bad.f_store_guid[kStoreIdentityRoleByte] = kStoreIdentityFileRole;
    require(!bad.valid_payload(), "all-zero F root accepted");
    auto zero_c = a;
    zero_c.c_store_guid.fill(0);
    require(!P50SourceArmMsg(zero_c).valid_payload(), "all-zero C root accepted");
}

void malformed_f_ack_rejected_at_ingress() {
    Fixture f; f.allocate();
    auto bad = f.acknowledged;
    bad.f_store_guid = f.request.arm.c_store_guid;
    bad.f_store_guid[kStoreIdentityRoleByte] |= kStoreIdentityFileRole;
    require(f.ingress->accept_f_source_armed(f.lease, bad, f.now) ==
                SourceIngressResult::InvalidAck,
            "malformed F ACK crossed ingress validation");
}

void capacity_rejection_is_terminal_and_cleanable() {
    Fixture f; f.owner = SourceIngressOwner(3); f.allocate();
    const std::array<uint8_t, 4> too_large{'x', 'x', 'x', 'x'};
    require(f.ingress->append_wrapper_bytes(f.lease, too_large, f.now) ==
                SourceIngressResult::Invalid, "capacity overflow accepted");
    require(f.ingress->state() == SourceIngressState::Rejected &&
                f.owner.cleanup(f.lease) == SourceIngressResult::Accepted,
            "capacity rejection was not safely cleanable");
}

void settlement_is_canonical_and_late() {
    Fixture f; f.allocate(); f.ready();
    require(f.finalize_cap.consume(), "finalize capability did not consume");
    auto start = f.ingress->start_cache_wire(f.lease, key(f), 9, 77, f.now);
    require(start.has_value(), "start material missing");
    require(f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxBegin, f.now) ==
                SourceIngressResult::Accepted &&
            f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxBody, f.now) ==
                SourceIngressResult::Accepted &&
            f.ingress->emit_cache_wire(f.lease, CacheWireFrame::TxCommit, f.now) ==
                SourceIngressResult::Accepted, "ordered CacheWire failed");
    auto bad = witness(f, *start, CacheWireSettlementDisposition::CommittedInput);
    bad.operation_id++;
    require(f.ingress->settle_cache_wire(f.lease, bad, f.now) ==
                SourceIngressResult::Invalid, "wrong operation settled commit");
    bad = witness(f, *start, CacheWireSettlementDisposition::CommittedInput);
    bad.disposition = static_cast<CacheWireSettlementDisposition>(99);
    require(f.ingress->settle_cache_wire(f.lease, bad, f.now) ==
                SourceIngressResult::Invalid, "future settlement disposition accepted");
    auto good = witness(f, *start, CacheWireSettlementDisposition::CommittedInput);
    require(f.ingress->settle_cache_wire(f.lease, good, f.now) ==
                SourceIngressResult::Accepted, "canonical commit rejected");
    require(f.owner.cleanup(f.lease) == SourceIngressResult::Accepted,
            "committed lease did not clean up");
}

void revocation_and_reconcile() {
    Fixture f; f.allocate(); f.source(); f.wait_ok();
    auto cap = f.ingress->take_source_finalize(f.lease, f.now);
    require(cap.has_value() && cap->valid(), "capability missing");
    SourceFinalize moved = std::move(*cap); f.finalize_cap = std::move(moved);
    f.refresh();
    require(f.ingress->cancel(f.lease, f.now) == SourceIngressResult::Accepted,
            "pre-wire cancel failed");
    require(!f.finalize_cap.valid() && !f.finalize_cap.consume(),
            "cancel did not revoke finalize capability");
    require(f.owner.cleanup(f.lease) == SourceIngressResult::Accepted,
            "cancelled lease did not clean up");

    Fixture r; r.allocate(); r.ready();
    require(r.finalize_cap.consume(), "reconcile finalize capability did not consume");
    auto start = r.ingress->start_cache_wire(r.lease, key(r), 1, 17, r.now);
    require(start.has_value(), "reconcile start failed");
    r.ingress->emit_cache_wire(r.lease, CacheWireFrame::TxBegin, r.now);
    require(r.ingress->cancel(r.lease, r.now) == SourceIngressResult::ReconcileRequired,
            "in-flight cancel did not reconcile");
    require(r.owner.cleanup(r.lease) == SourceIngressResult::NotReady,
            "reconcile cleaned before settlement");
    auto pre = witness(r, *start, CacheWireSettlementDisposition::ProvedPreDurable);
    require(r.ingress->settle_cache_wire(r.lease, pre, r.now) ==
                SourceIngressResult::Accepted &&
            r.owner.cleanup(r.lease) == SourceIngressResult::Accepted,
            "pre-durable reconcile failed");
}

}  // namespace

static_assert(!std::is_copy_constructible_v<SourceFinalize>);
static_assert(!std::is_copy_assignable_v<SourceFinalize>);

int main() {
    try {
        two_latches_and_order();
        eof_waitpid_and_owner_fences();
        deadline_is_nonrenewable();
        root_alias_rejected();
        malformed_f_ack_rejected_at_ingress();
        capacity_rejection_is_terminal_and_cleanable();
        settlement_is_canonical_and_late();
        duplicate_cachewire_frame_reconciles();
        revocation_and_reconcile();
        std::cout << "ok - hardened C source ingress successor\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
