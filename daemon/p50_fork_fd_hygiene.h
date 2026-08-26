#pragma once

// Exact descriptor ownership for a long-lived compiler child.  This is a
// first-fork contract: a child may proceed only after it has proved that the
// two legacy channels (result pipe and client socket), and optionally the one
// authenticated P50 source descriptor, are valid and distinct and that every
// other inherited descriptor was really closed.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace icecc::p50::forkfd {

struct SourceIdentity {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t mode = 0;
    uint64_t size = 0;
    uint64_t seals = 0;
    bool has_seals = false;

    [[nodiscard]] bool valid() const noexcept {
        return device != 0 && inode != 0 && mode != 0;
    }
};

class ForkSourceLease;
struct KeepSet;
struct Result;

// This token is minted by the delivery owner, not by the fork seam.  Its
// constructor is private so an arbitrary fd/nonzero-id pair cannot be made
// admissible by a caller that merely knows the wire DeliveryId.  A future
// live cache-delivery owner must add the bounded minting adapter at its own
// ownership boundary; this slice intentionally does not fabricate that owner.
class DeliveryOwnerToken final {
public:
    DeliveryOwnerToken() = delete;
    DeliveryOwnerToken(const DeliveryOwnerToken&) = delete;
    DeliveryOwnerToken& operator=(const DeliveryOwnerToken&) = delete;
    DeliveryOwnerToken(DeliveryOwnerToken&& other) noexcept;
    DeliveryOwnerToken& operator=(DeliveryOwnerToken&& other) noexcept;
    ~DeliveryOwnerToken();

private:
    DeliveryOwnerToken(int expected_fd, uint64_t expected_delivery_id,
                       int owned_proof_fd, int control_fd, SourceIdentity identity,
                       uint64_t owner_cookie, int proof_slot_fd = -1) noexcept;
    int expected_fd_ = -1;
    uint64_t expected_delivery_id_ = 0;
    // This duplicate is owned by the token.  The delivery handoff itself is
    // never stored here and is therefore never closed by token destruction.
    int owned_proof_fd_ = -1;
    // A second, hidden handle to the proof's open-file description.  The
    // proof descriptor number can be closed and reused by its caller; this
    // control handle lets retirement compare the actual kernel OFD and fail
    // closed before touching a replacement.
    int control_fd_ = -1;
    // Test-only observation slot.  It is deliberately not an ownership
    // handle: a delivery caller may close and reuse this number, including
    // with dup3() of the borrowed handoff.  The private proof/control pair
    // below remains the sole retirement authority.
    int proof_slot_fd_ = -1;
    SourceIdentity identity_{};
    uint64_t owner_cookie_ = 0;

    friend std::optional<DeliveryOwnerToken>
    test_make_delivery_owner(int expected_fd, uint64_t expected_delivery_id) noexcept;
    friend int test_delivery_owner_proof_fd(const DeliveryOwnerToken&) noexcept;
    friend std::optional<DeliveryOwnerToken>
    test_make_delivery_owner_alias(int expected_fd,
                                   uint64_t expected_delivery_id) noexcept;
    friend std::optional<DeliveryOwnerToken>
    test_make_delivery_owner_control_alias(int expected_fd,
                                            uint64_t expected_delivery_id) noexcept;
    friend void test_disarm_delivery_owner(DeliveryOwnerToken&) noexcept;
    friend std::optional<ForkSourceLease>
    mint_fork_source_lease(DeliveryOwnerToken&& owner, int fd,
                           uint64_t delivery_id) noexcept;
};

enum class Failure : uint8_t {
    None = 0,
    InvalidKeepSet,
    OwnershipFailure,
    TypeFailure,
    EnumerationFailure,
    ParseFailure,
    CloseFailure,
    BackendUnavailable,
};

class ForkSourceLease final {
public:
    ForkSourceLease() = delete;
    ForkSourceLease(const ForkSourceLease&) = delete;
    ForkSourceLease& operator=(const ForkSourceLease&) = delete;
    ForkSourceLease(ForkSourceLease&& other) noexcept;
    ForkSourceLease& operator=(ForkSourceLease&& other) noexcept;
    ~ForkSourceLease();

    [[nodiscard]] int fd() const noexcept { return borrowed_handoff_fd_; }
    [[nodiscard]] uint64_t delivery_id() const noexcept { return delivery_id_; }
    [[nodiscard]] bool valid() const noexcept {
        return borrowed_handoff_fd_ >= 0 && delivery_id_ != 0 && owner_cookie_ != 0 &&
               owned_proof_fd_ >= 0 && control_fd_ >= 0 && identity_.valid() &&
               borrowed_handoff_fd_ == expected_fd_ && delivery_id_ == expected_delivery_id_;
    }
    [[nodiscard]] bool identity_matches_current() const noexcept;

private:
    ForkSourceLease(int fd, uint64_t delivery_id, int expected_fd,
                    uint64_t expected_delivery_id, int owned_proof_fd,
                    int control_fd, SourceIdentity identity,
                    uint64_t owner_cookie, int proof_slot_fd = -1) noexcept;
    // The handoff descriptor is borrowed from the delivery caller.  A lease
    // may validate it and keep its number, but must never close it (including
    // when move-assignment replaces this lease).
    int borrowed_handoff_fd_ = -1;
    uint64_t delivery_id_ = 0;
    int expected_fd_ = -1;
    uint64_t expected_delivery_id_ = 0;
    // This independent duplicate is owned by the lease and is the only
    // descriptor its destructor may retire.
    int owned_proof_fd_ = -1;
    int control_fd_ = -1;
    // See DeliveryOwnerToken::proof_slot_fd_.  This slot is never closed by
    // lease retirement or move/destruction.
    int proof_slot_fd_ = -1;
    SourceIdentity identity_{};
    uint64_t owner_cookie_ = 0;

    bool retire_identity_proof() noexcept;

    friend std::optional<ForkSourceLease>
    mint_fork_source_lease(DeliveryOwnerToken&& owner, int fd,
                           uint64_t delivery_id) noexcept;
    friend Result sweep(KeepSet& keep) noexcept;
    friend int test_fork_source_lease_proof_fd(const ForkSourceLease&) noexcept;
};

// The only public construction seam is deliberately fed by an opaque token
// minted by the delivery owner.  No production minting implementation exists
// in this bounded slice; the daemon therefore fails closed until its owner
// bridge supplies one.
[[nodiscard]] std::optional<ForkSourceLease>
mint_fork_source_lease(DeliveryOwnerToken&& owner, int fd,
                       uint64_t delivery_id) noexcept;

struct KeepSet {
    int stat_pipe_fd = -1;
    int client_fd = -1;
    std::optional<ForkSourceLease> source;
    // A P50 admission or any raw source candidate makes a source lease
    // mandatory.  This prevents the legacy two-FD shape from silently
    // accepting an unbound P50 descriptor.
    bool source_required = false;
    // When the legacy daemon still holds a raw candidate, this optional value
    // binds the move-only lease to that exact descriptor number.  A future
    // owner can omit it when the lease itself owns the descriptor.
    std::optional<int> expected_source_fd;
};

struct Result {
    Failure failure = Failure::None;
    size_t closed_count = 0;

    [[nodiscard]] bool ok() const noexcept { return failure == Failure::None; }
};

[[nodiscard]] Result sweep(KeepSet& keep) noexcept;
[[nodiscard]] const char* failure_name(Failure failure) noexcept;

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
// These hooks are deliberately test-only.  They inject failures at the
// backend boundary without replacing libc globally, so deletion-sensitive
// tests exercise the exact production decision tree.
struct TestHooks {
    bool force_close_range_unsupported = false;
    bool force_close_range_failure = false;
    bool force_proc_failure = false;
    bool force_parse_failure = false;
    bool force_proc_close_ebadf = false;
    // If non-zero, make the test-only kcmp seam fail with this errno.  The
    // production path has no fallback for OFD comparison; retirement must
    // nevertheless dispose of its private ownership handles safely.
    int force_kcmp_errno = 0;
    // Number of successful comparison calls to permit before injecting the
    // errno above.  This lets fork tests reach retirement after validation.
    int force_kcmp_after_calls = 0;
    int fail_close_fd = -1;
};

void set_test_hooks(TestHooks hooks) noexcept;
void reset_test_hooks() noexcept;

std::optional<DeliveryOwnerToken>
test_make_delivery_owner(int expected_fd, uint64_t expected_delivery_id) noexcept;
int test_delivery_owner_proof_fd(const DeliveryOwnerToken&) noexcept;
int test_fork_source_lease_proof_fd(const ForkSourceLease&) noexcept;
std::optional<DeliveryOwnerToken>
test_make_delivery_owner_alias(int expected_fd,
                               uint64_t expected_delivery_id) noexcept;
std::optional<DeliveryOwnerToken>
test_make_delivery_owner_control_alias(int expected_fd,
                                       uint64_t expected_delivery_id) noexcept;
void test_disarm_delivery_owner(DeliveryOwnerToken&) noexcept;
#endif

} // namespace icecc::p50::forkfd
