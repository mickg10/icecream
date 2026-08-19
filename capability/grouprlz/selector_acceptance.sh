#!/usr/bin/env bash
# selector_acceptance.sh — run the acceptance set in one command.
#
# The acceptance set is the ELEVEN verdict-carriers, named by local-oracle, plus the gates
# that are added later by explicit ruling.  It is deliberately NOT "every script in this
# directory": the other ~35 are historical experiments, and sweeping them in would turn a pile
# of old probes into an accidental product specification.  Anything added here should be added
# because someone ruled it part of acceptance, not because it happened to exist.
#
#   C++ gates      p29_online_s1_test.cpp        T_current/full-route equivalence
#                  p29_block_catalogue_test.cpp  shared canonical Block id space
#                  p29_sparse_fblocks_test.cpp   F's hole-tolerant Block store
#                  p29_prepare_commit_test.cpp   prepare/commit/abort, gates 2-6
#                  p29_transfer_transaction_test.cpp exact retry identity + retained Ack
#                  mo_factor_transaction_test.cpp MO dictionary whole-TU rollback
#                  p29_shared_authority_test.cpp multi-C admission + per-F single-flight
#                  p29_two_f_state_test.cpp      two-F route/COPY/epoch/retry state
#                  p29_event_record_test.cpp     unified replay row and exact invariants
#                  p29_routing_replay_test.cpp   chronological R0-R5 reference replay
#   fault runners  p29_journal_mutations.sh          the journal's failure paths
#                  selector_equivalence_mutations.sh both byte-equivalence gates' paths
#                  selector_1f_costing_mutations.sh  the 1F launcher's own failure path
#                  selector_live_source_mutations.sh source-level selector attribution faults
#                  selector_transaction_source_mutations.sh whole-state abort/Ack-order faults
#                  p29_shared_authority_mutations.sh shared-authority failure paths
#                  p29_two_f_state_mutations.sh      two-F route/F boundary paths
#   chronology     p29_unavailable_suffix.sh         TU2 absent through TU1 Ack
#                  selector_prefix_invariance.sh     physical prefixes vs supplied suffix
#   wire gates     selector_step1_equivalence.sh  planning vs on-demand, byte-identical
#                  selector_step2_equivalence.sh  reference vs build under test
#                  selector_tag_regression.sh     typed-tag guards G1-G3
#                  selector_transaction_retry.sh whole-TU reject/retry + retained Ack
#                  selector_1f_costing.sh         per-TU candidate costing (differential basis)
#                  selector_live_history.sh       the LIVE selected history (added by ruling)
#
# FAIL CLOSED, and the whole point is that it can fail: every step's EXIT STATUS is required,
# a step that fails aborts the run non-zero, and the summary is printed from recorded results
# rather than from "we got to the end".  Run with SELECTOR_ACCEPTANCE_SELFTEST=1 to inject a
# failing step and confirm the runner reports and propagates it.
#
# Usage:
#   BIN=<codec50-sink>  REF_BIN=<reference codec50-sink for step 2>  ./selector_acceptance.sh
#   MX=<ii-matrix>  WORK=<scratch>  CELL=<project/profile for the tag+manifest gates>
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MX=${MX:-$HOME/ictmp/ii-matrix}
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-$(mktemp -d /tmp/selacc.XXXXXX)}
CELL=${CELL:-fmt/debian-gcc}
CXX_OPT=${CXX_OPT:--std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror}
CXX_SAN=${CXX_SAN:--std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic -Werror}
CXX_TSAN=${CXX_TSAN:--std=c++17 -O1 -g -fsanitize=thread -fno-pie -no-pie -Wall -Wextra -Wpedantic -Werror}

NAMES=(); STATUS=()

die() { echo "ACCEPTANCE ABORTED: $*" >&2; exit 1; }
mkdir -p "$WORK/logs"

[ -x "$BIN" ] || die "codec binary not executable: $BIN (build with ./selector_build_codec50_sink.sh)"
[ -d "$MX" ] || die "ii-matrix not found: $MX"
# Required, not silently skipped: step 2 compares a reference build against the build under
# test, and a comparison with nothing to compare against is not a weaker gate, it is no gate.
#
# But "REF_BIN is executable" does not make it a REFERENCE.  Pointing it at the build under
# test turns step 2 into current-vs-current -- which passes trivially and still reports every
# step green.  So the reference commit is NAMED here, and a REF_BIN byte-identical to BIN is
# rejected outright.  Both binaries' SHA-256s go into the log, so what a green run compared is
# recoverable from the record rather than from whoever ran it.
REF_COMMIT=749ca47c7ed21b1997cd87c81158e85d5bb4c653   # T_current step 1: on-demand one-TU literals
[ -n "${REF_BIN:-}" ] && [ -x "$REF_BIN" ] || die \
"REF_BIN must point at a build of the step-1 reference commit
     $REF_COMMIT
   Build one with:
     git show $REF_COMMIT:capability/grouprlz/codec50-sink.cpp > <dir>/codec50-sink.cpp
     cp $HERE/*.h <dir>/ && (cd <dir> && g++ -O3 -march=native -std=c++17 -fopenmp \\
        -DWITH_BSC_GROUPS -I. -I\$HOME/libbsc/libbsc codec50-sink.cpp -o codec50-sink \\
        \$HOME/grouprlz/libbsc.a /usr/lib/x86_64-linux-gnu/libzstd.a -lz -lpthread)"
cmp -s "$REF_BIN" "$BIN" && die \
"REF_BIN is byte-identical to BIN, so step 2 would compare the build under test with itself.
   REF_BIN must be a build of $REF_COMMIT."
BIN_SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
REF_SHA=$(sha256sum "$REF_BIN" | cut -d' ' -f1)

step() { # step <name> -- <command...>
  local name=$1; shift 2
  local log=$WORK/logs/$name.log
  local rc=0
  printf '=== %s\n' "$name"
  "$@" >"$log" 2>&1 || rc=$?
  NAMES+=("$name"); STATUS+=("$rc")
  if [ "$rc" -ne 0 ]; then
    echo "    FAILED (exit $rc); last lines:" >&2
    tail -20 "$log" | sed 's/^/    /' >&2
    summary; exit 1
  fi
  tail -1 "$log" | sed 's/^/    /'
}

summary() {
  echo
  printf 'build under test  %s  sha256=%s\n' "$BIN" "$BIN_SHA"
  printf 'step-1 reference  %s  sha256=%s\n' "$REF_BIN" "$REF_SHA"
  printf 'reference commit  %s\n' "$REF_COMMIT"
  echo
  printf '%-38s %s\n' STEP RESULT
  local i
  for i in "${!NAMES[@]}"; do
    printf '%-38s %s\n' "${NAMES[$i]}" "$([ "${STATUS[$i]}" -eq 0 ] && echo PASS || echo "FAIL(${STATUS[$i]})")"
  done
  echo
  echo "logs: $WORK/logs"
}

# --- a manifest for the gates that take one -----------------------------------------------
P=${CELL%%/*}; PR=${CELL##*/}
J=$MX/$P/$PR/corpus.json
[ -f "$J" ] || die "no corpus.json at $J"
meta=$(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])" "$J")
read -r REL SHA N <<<"$meta"
have=$(sha256sum "$MX/$P/$PR/$REL" | cut -d' ' -f1)
[ "$have" = "$SHA" ] || die "payload sha256 mismatch for $CELL"
mkdir -p "$WORK/ii"
zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C "$WORK/ii"
find "$WORK/ii" -name '*.ii' | sort >"$WORK/man1"
M=$(wc -l <"$WORK/man1"); [ "$M" = "$N" ] || die "extracted $M .ii files, corpus.json says $N"
: >"$WORK/man4"; for i in 1 2 3 4; do cat "$WORK/man1" >>"$WORK/man4"; done

# --- C++ gates, optimized and sanitized ----------------------------------------------------
cxx_gate() { # cxx_gate <test.cpp> <flags> <outname>
  local src=$1 flags=$2 out=$3
  g++ $flags -pthread "$HERE/$src" -o "$WORK/$out" || return 1
  "$WORK/$out"
}
cxx_tsan_gate() { # TSan needs a non-random address layout on some Linux kernels.
  local src=$1 flags=$2 out=$3 arch
  g++ $flags -pthread "$HERE/$src" -o "$WORK/$out" || return 1
  arch=$(uname -m)
  if command -v setarch >/dev/null 2>&1 && setarch "$arch" -R true >/dev/null 2>&1; then
    setarch "$arch" -R "$WORK/$out"
  else
    "$WORK/$out"
  fi
}
for t in p29_online_s1_test p29_block_catalogue_test p29_sparse_fblocks_test p29_prepare_commit_test \
         p29_transfer_transaction_test mo_factor_transaction_test p29_shared_authority_test \
         p29_two_f_state_test p29_event_record_test p29_routing_replay_test; do
  step "$t (opt)" -- cxx_gate "$t.cpp" "$CXX_OPT" "$t.opt"
  step "$t (asan+ubsan)" -- cxx_gate "$t.cpp" "$CXX_SAN" "$t.san"
done
step "p29_shared_authority_test (tsan)" -- cxx_tsan_gate p29_shared_authority_test.cpp \
     "$CXX_TSAN" p29_shared_authority_test.tsan
step "p29_two_f_state_test (tsan)" -- cxx_tsan_gate p29_two_f_state_test.cpp \
     "$CXX_TSAN" p29_two_f_state_test.tsan

# --- fault runners: the gates' own failure paths -------------------------------------------
step p29_journal_mutations -- env WORK="$WORK/mut.journal" "$HERE/p29_journal_mutations.sh"
step selector_equivalence_mutations -- env BIN="$BIN" S1REF="$REF_BIN" MX="$MX" \
     WORK="$WORK/mut.equiv" "$HERE/selector_equivalence_mutations.sh" "$CELL"
step selector_1f_costing_mutations -- env BIN="$BIN" MX="$MX" \
     WORK="$WORK/mut.costing" "$HERE/selector_1f_costing_mutations.sh" "$CELL"
step selector_live_source_mutations -- env BIN="$BIN" WORK="$WORK/mut.selector-source" \
     "$HERE/selector_live_source_mutations.sh" "$WORK/man1"
step selector_transaction_source_mutations -- env BIN="$BIN" WORK="$WORK/mut.transaction-source" \
     "$HERE/selector_transaction_source_mutations.sh" "$WORK/man1"
step p29_shared_authority_mutations -- env WORK="$WORK/mut.shared-authority" \
     "$HERE/p29_shared_authority_mutations.sh"
step p29_two_f_state_mutations -- env WORK="$WORK/mut.two-f" \
     "$HERE/p29_two_f_state_mutations.sh"

# --- current-input chronology ---------------------------------------------------------------
step p29_unavailable_suffix -- env WORK="$WORK/unavailable-suffix" \
     "$HERE/p29_unavailable_suffix.sh"
step selector_prefix_invariance -- env BIN="$BIN" WORK="$WORK/prefix-invariance" \
     "$HERE/selector_prefix_invariance.sh" "$WORK/man1"

# --- wire gates ----------------------------------------------------------------------------
step selector_tag_regression -- env BIN="$BIN" "$HERE/selector_tag_regression.sh" "$WORK/man4" "$WORK/man1"
step selector_transaction_retry -- env BIN="$BIN" WORK="$WORK/transaction-retry" \
     "$HERE/selector_transaction_retry.sh" "$WORK/man1"
step selector_step1_equivalence -- env BIN="$BIN" MX="$MX" \
     WORK="$WORK/step1" OUT="$WORK/step1/evidence" "$HERE/selector_step1_equivalence.sh"
step selector_step2_equivalence -- env S1="$REF_BIN" S2="$BIN" MX="$MX" \
     WORK="$WORK/step2" OUT="$WORK/step2/evidence" "$HERE/selector_step2_equivalence.sh"
step selector_1f_costing -- env BIN="$BIN" MX="$MX" \
     WORK="$WORK/costing" OUT="$WORK/costing/results" "$HERE/selector_1f_costing.sh"
step selector_live_history -- env BIN="$BIN" MX="$MX" \
     WORK="$WORK/livehist" OUT="$WORK/livehist/results" "$HERE/selector_live_history.sh"

# A runner whose failure path is never exercised is the thing this lane keeps finding.  This
# injects a failing step on demand so the propagation can be checked rather than assumed.
if [ "${SELECTOR_ACCEPTANCE_SELFTEST:-0}" = 1 ]; then
  step injected_failure -- false
fi

summary
echo "acceptance set: ${#NAMES[@]}/${#NAMES[@]} steps passed"
