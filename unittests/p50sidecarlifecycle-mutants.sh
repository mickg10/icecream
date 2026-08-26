#!/bin/sh
set -eu
root="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}"
build="${ICECC_TEST_BUILDDIR:-$(pwd)}"
cxx="${ICECC_TEST_CXX:-c++}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/p50-sidecar-lifecycle-mutant.XXXXXX")"
trap 'rm -r -- "$tmp"' EXIT HUP INT TERM
expected_mutants=17
mutant_count=0
compiled_count=0
for mutant_name in pgid store-generation allocator-store-generation lease-store-generation ready-generation-key ready-generation-assignment stale-ready listener-node teardown-deadline legacy-direct owner-key group-domain fabricated-lease over-capacity discarded-registration socket-substitution group-proof-required; do
    mutant_count=$((mutant_count + 1))
    cp "$root/cache/p50_sidecar_lifecycle.cpp" "$tmp/mutant.cpp"
    # These are semantic deletion mutants; the runtime witness must redden
    # each one rather than merely matching source text.
    if test "$mutant_name" = pgid; then
        sed -i 's/observation\.observed_pgid != process_group_/false/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = store-generation; then
        sed -i 's/observation\.store_generation != identity_->store_generation/false/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = allocator-store-generation; then
        sed -i 's/identity\.store_generation = allocated->store_generation;/identity.store_generation = config_.control_generation;/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = lease-store-generation; then
        sed -i 's/lease\.store_generation != identity_->store_generation/false/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = ready-generation-key; then
        sed -i 's/F_STORE_GENERATION=/STORE_GENERATION=/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = ready-generation-assignment; then
        sed -i 's/lease\.store_generation = store_generation;/lease.store_generation = 0;/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = stale-ready; then
        sed -i 's/leader_waitable_ || leader_reaped_/false/' "$tmp/mutant.cpp"
        sed -i 's/(leader_waitable_ || identity_lost_)/(false)/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = listener-node; then
        sed -i 's/return observation\.observed_device == identity_->listener_device/return true || observation.observed_device == identity_->listener_device/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = teardown-deadline; then
        sed -i 's/return teardown_started_ && now >= teardown_deadline_/return false/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = legacy-direct; then
        sed -i '0,/if (state_ != LifecycleState::TerminatingGrace/s//if (false \&\& state_ != LifecycleState::TerminatingGrace/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = group-domain; then
        perl -0pi -e 's@bool SidecarLifecycle::exact_group_absent\(\n    const LifecycleObservation& observation\) const noexcept \{.*?\n\}@bool SidecarLifecycle::exact_group_absent(\n    const LifecycleObservation& observation) const noexcept {\n    return leader_reaped_ && observation.group == GroupObservation::Gone &&\n           observation.observed_pgid == process_group_;\n}@s' "$tmp/mutant.cpp"
    elif test "$mutant_name" = fabricated-lease; then
        perl -0pi -e 's/group_domain_ = kill_domain_verifier_->capture\(child_pid_,\s*process_group_\);/group_domain_ = observation.group_domain;/s' "$tmp/mutant.cpp"
    elif test "$mutant_name" = over-capacity; then
        sed -i 's/state_->owners.size() >= SharedState::kMaximumOwners/false/g' "$tmp/mutant.cpp"
        sed -i 's/state_->slots.size() >= SharedState::kMaximumOwners/false/g' "$tmp/mutant.cpp"
    elif test "$mutant_name" = discarded-registration; then
        sed -i '0,/state_->owners.erase(iterator);/s//if (false) state_->owners.erase(iterator);/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = socket-substitution; then
        # Keep the substitution bypass syntactically valid so the behavioral
        # witness, rather than the compiler, kills this true mutant.
        sed -i '/if (::lstat(lease.socket_path.c_str(), \&listener) != 0 ||/,/listener.st_ino != lease.listener_inode)/c\    if (false)' "$tmp/mutant.cpp"
    elif test "$mutant_name" = group-proof-required; then
        # A permissive observation must not authorize teardown before the
        # reducer has captured a group lease for the fork.  Restore the
        # fail-closed branch to true to make this semantic mutant executable.
        perl -0pi -e 's/if \(!group_proof_required_\)\n        return false;/if (!group_proof_required_)\n        return true;/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = owner-key; then
        sed -i 's/event\.owner != owner_key()/false/' "$tmp/mutant.cpp"
    else
        echo "unknown lifecycle mutant: $mutant_name" >&2
        exit 1
    fi
    "$cxx" -std=c++20 -I"$root" -I"$root/cache" -I"$root/services" ${ICECC_TEST_CXXFLAGS:-} \
        -c "$tmp/mutant.cpp" -o "$tmp/mutant.o"
    compiled_count=$((compiled_count + 1))
    "$cxx" -std=c++20 -pthread -I"$root" -I"$root/services" \
        "$root/unittests/p50_sidecar_lifecycle_test.cpp" "$tmp/mutant.o" \
        "$build/../cache/libp50sidecarsupervisor.a" "$build/../cache/libp50localtransport.a" \
        "$build/../services/.libs/libicecc.a" \
        -L"${P50_R2_DEPS:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-worktrees/.p50-r2-deps/root/usr/lib/x86_64-linux-gnu}" \
        -lxxhash ${ICECC_TEST_LIBS:-} -o "$tmp/mutant"
    if "$tmp/mutant" >/dev/null 2>&1; then
        echo "$mutant_name lifecycle mutant survived" >&2
        exit 1
    fi
done
test "$mutant_count" -eq "$expected_mutants"
test "$compiled_count" -eq "$expected_mutants"
