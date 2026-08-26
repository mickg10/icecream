#!/bin/sh
set -eu
root="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}"
build="${ICECC_TEST_BUILDDIR:-$(pwd)}"
cxx="${ICECC_TEST_CXX:-c++}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/p50-sidecar-lifecycle-mutant.XXXXXX")"
trap 'rm -r -- "$tmp"' EXIT HUP INT TERM
for mutant_name in pgid store-generation stale-ready listener-node teardown-deadline legacy-direct owner-key group-domain; do
    cp "$root/cache/p50_sidecar_lifecycle.cpp" "$tmp/mutant.cpp"
    # These are semantic deletion mutants; the runtime witness must redden
    # each one rather than merely matching source text.
    if test "$mutant_name" = pgid; then
        sed -i 's/observation\.observed_pgid == process_group_/true/' "$tmp/mutant.cpp"
    elif test "$mutant_name" = store-generation; then
        sed -i 's/observation\.store_generation != identity_->store_generation/false/' "$tmp/mutant.cpp"
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
        sed -i 's/group_domain_->valid()/true/' "$tmp/mutant.cpp"
        sed -i 's/observation.group_domain.valid()/true/' "$tmp/mutant.cpp"
        sed -i 's/observation.group_domain == \*group_domain_/true/' "$tmp/mutant.cpp"
    else
        sed -i 's/event\.owner != owner_key()/false/' "$tmp/mutant.cpp"
    fi
    "$cxx" -std=c++20 -I"$root" -I"$root/cache" -I"$root/services" ${ICECC_TEST_CXXFLAGS:-} \
        -c "$tmp/mutant.cpp" -o "$tmp/mutant.o"
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
