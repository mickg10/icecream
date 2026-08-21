#!/usr/bin/env bash
# Prove that the A2 fixture rejects broken canonical publication, logical-request replay,
# exact-input matching, per-F single-flight and relationship ownership/independence.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$(mktemp -d /tmp/p29-shared-authority-mut.XXXXXX)}
CXXFLAGS=${CXXFLAGS:-${ICE_CXX_STANDARD_FLAG:--std=c++23} -O2 -pthread -Wall -Wextra -Wpedantic -Werror}

fail(){ echo "SHARED AUTHORITY MUTATION FAIL: $*" >&2; exit 1; }
trap 'rc=$?; [ "$rc" -eq 0 ] || echo "  evidence retained: $WORK" >&2' EXIT
mkdir -p "$WORK"

build_and_run(){ # build_and_run <directory>
  local dir=$1
  cp "$HERE/p29_shared_authority_test.cpp" "$dir/"
  cp "$HERE/p29_online_s1.h" "$HERE/p29_transfer_transaction.h" "$dir/"
  g++ $CXXFLAGS "$dir/p29_shared_authority_test.cpp" -o "$dir/test" \
      >"$dir/build.out" 2>"$dir/build.err" || { echo BUILD_ERROR; return; }
  if "$dir/test" >"$dir/run.out" 2>"$dir/run.err"; then echo PASS; else echo FAIL; fi
}

mkdir -p "$WORK/control"
cp "$HERE/p29_shared_authority.h" "$WORK/control/"
[ "$(build_and_run "$WORK/control")" = PASS ] || fail "unmodified control did not pass"
echo "control: unmodified shared authority passes"

mutate(){ # mutate <name> <expected diagnostic> <sed expression>...
  local name=$1 expected=$2; shift 2
  local dir=$WORK/$name header=$WORK/$name/p29_shared_authority.h
  mkdir -p "$dir"
  cp "$HERE/p29_shared_authority.h" "$header"
  for expression in "$@"; do sed -i "$expression" "$header"; done
  cmp -s "$HERE/p29_shared_authority.h" "$header" && fail "$name changed no source"
  local result
  result=$(build_and_run "$dir")
  [ "$result" = FAIL ] || fail "$name was not rejected (result=$result)"
  grep -qF "$expected" "$dir/run.err" || fail "$name failed, but not at its intended check"
  printf 'caught %-22s %s\n' "$name" "$expected"
}

# Equal content no longer reuses its canonical table entry.
mutate canonical_split 'same atoms received different canonical IDs' \
  's/if (found != ids_\.end()) return {found->second, false};/if (false \&\& found != ids_.end()) return {found->second, false};/' \
  's/if (!inserted\.second) {/if (false \&\& !inserted.second) {/'

# A retry erases its request-index entry immediately before lookup and is admitted again.
mutate logical_retry_reapplied 'logical retry did not produce one Created plus one Existing admission' \
  's/        const auto existing = requests_\.find(input\.producer_request_id);/        requests_.erase(input.producer_request_id);\n        const auto existing = requests_.find(input.producer_request_id);/'

# Same logical request with changed exact closure is reported as an ordinary retry.
mutate changed_request_accepted 'changed retry extent was not identified' \
  's/same ? AdmissionResult::Existing : AdmissionResult::RequestMismatch/same ? AdmissionResult::Existing : AdmissionResult::Existing/'

# Both concurrent producers believe they own the same F/object publication.
mutate duplicate_install_owner 'concurrent F1 demand did not single-flight' \
  's/return {ReserveResult::Joined, pending->second};/return {ReserveResult::Owner, pending->second};/'

# The authority hands each frontend a new relationship object instead of retaining one owner.
mutate ephemeral_relationship 'two frontends received different F1 relationship owners' \
  's/if (found != relationships_\.end()) return found->second;/if (false \&\& found != relationships_.end()) return found->second;/'

# Every F/epoch key aliases one relationship, so committed F1 state appears at F2.
mutate merged_f_replicas 'F1 state leaked into another F or cache epoch' \
  's/return f_id == other\.f_id \&\& cache_epoch == other\.cache_epoch;/return (static_cast<void>(other), true);/' \
  '/const uint64_t hash = Opaque128Hash{}(key\.f_id);/c\        const uint64_t hash = key.cache_epoch - key.cache_epoch;' \
  's/return static_cast<size_t>(hash \^ (key\.cache_epoch \* 0x9e3779b97f4a7c15ULL));/return static_cast<size_t>(hash);/'

mutate unpublished_children 'an unpublished canonical child was accepted' \
  's/if (atom >= atoms_\.size())/if (false \&\& atom >= atoms_.size())/' \
  's/if (region >= regions_\.size())/if (false \&\& region >= regions_.size())/'

echo "all 7 shared-authority mutations reached their intended checks"
