#!/usr/bin/env bash
# Prove that the Phase-B fixture observes the route/F boundary it claims: local COPY proof,
# independent route lanes, representation-independent route commit, global-vs-route plan,
# source eviction and retained-Ack single application.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$(mktemp -d /tmp/p29-two-f-mut.XXXXXX)}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror}

fail(){ echo "TWO-F MUTATION FAIL: $*" >&2; exit 1; }
trap 'rc=$?; [ "$rc" -eq 0 ] || echo "  evidence retained: $WORK" >&2' EXIT
mkdir -p "$WORK"

copy_sources(){
  local dir=$1
  cp "$HERE/p29_two_f_state_test.cpp" "$HERE/p29_two_f_state.h" \
     "$HERE/p29_event_record.h" \
     "$HERE/p29_shared_authority.h" "$HERE/p29_online_s1.h" \
     "$HERE/p29_transfer_transaction.h" "$HERE/p29_sparse_blocks.h" "$dir/"
}

build_and_run(){
  local dir=$1
  g++ $CXXFLAGS "$dir/p29_two_f_state_test.cpp" -o "$dir/test" \
      >"$dir/build.out" 2>"$dir/build.err" || { echo BUILD_ERROR; return; }
  if "$dir/test" >"$dir/run.out" 2>"$dir/run.err"; then echo PASS; else echo FAIL; fi
}

mkdir -p "$WORK/control"
copy_sources "$WORK/control"
[ "$(build_and_run "$WORK/control")" = PASS ] || fail "unmodified control did not pass"
echo "control: unmodified two-F fixture passes"

mutate(){ # mutate <name> <file> <expected diagnostic> <sed expression>...
  local name=$1 file=$2 expected=$3; shift 3
  local dir=$WORK/$name target=$WORK/$name/$file
  mkdir -p "$dir"
  copy_sources "$dir"
  for expression in "$@"; do sed -i "$expression" "$target"; done
  cmp -s "$HERE/$file" "$target" && fail "$name changed no source"
  local result
  result=$(build_and_run "$dir")
  [ "$result" = FAIL ] || fail "$name was not rejected (result=$result)"
  grep -qF "$expected" "$dir/run.err" ||
    fail "$name failed, but not at its intended check"
  printf 'caught %-25s %s\n' "$name" "$expected"
}

# A candidate relabelled for F2 can use F1's route-local source coordinate.
mutate cross_route_copy p29_two_f_state.h \
  "F2 accepted a COPY proof whose outer candidate was relabelled for F2" \
  's/if (proof\.lane != lane_ ||/if (false ||/'

# An old internal prepare ticket resolves a newer prepare at the same durable sequence.
mutate stale_route_ticket p29_shared_authority.h \
  "stale route ticket was accepted for a later prepare" \
  's/lane\.pending->prepare_token != prepared\.prepare_token ||/false ||/'

# All F/lane keys alias one matcher, so F1's committed route is visible before F2 starts.
mutate merged_route_lanes p29_shared_authority.h \
  "C route history and F occurrence history diverged" \
  's/return f == other\.f && lane_id == other\.lane_id;/static_cast<void>(other); return true;/' \
  's/const uint64_t hash = FCacheIdentityHash{}(key\.f);/const uint64_t hash = 0;/'

# Ack installs objects but aborts semantic route history.  RAW is the first discriminating
# representation, so the per-transfer check observes this before a later prepare cascades.
mutate route_not_committed p29_shared_authority.h \
  "successful TU did not advance route commit sequence exactly once" \
  's/lane\.matcher\.commit();/lane.matcher.abort();/g' \
  '/++lane\.committed_sequence;/d'

# GLOBAL_S1 is silently built from the selected F's private route plan.
mutate global_uses_route_plan p29_two_f_state.h \
  "F1 fixture did not expose a GLOBAL-only explicit Block" \
  's/: pending\.admitted->global_plan;/: pending.route.plan;/'

# Eviction reports success but leaves every source position resident.
mutate source_eviction_ignored p29_two_f_state.h \
  "source eviction did not force an explicit Block definition" \
  's/resident_\[index\] = 0;/resident_[index] = 1;/'

# Retained-Ack recovery reapplies the already committed F transaction.
mutate retained_ack_reapplies p29_two_f_state.h \
  "lost-Ack recovery applied the TU a second time" \
  '/return retained;/i\        ++f_apply_count_;'

echo "all 7 two-F mutations reached their intended checks"
