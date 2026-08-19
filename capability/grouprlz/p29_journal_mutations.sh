#!/usr/bin/env bash
# p29_journal_mutations.sh — prove the prepare/commit/abort gates can FAIL, by breaking the
# journal in p29_online_s1.h itself and requiring the test to notice.
#
# The mutations are applied to a COPY of the production header and the unmodified test is
# compiled against it, so what is being gated is the real undo(), the real install_anchor()
# and the real admit() -- not a paraphrase of them living in the test.
#
# There is deliberately NO mutation for the boundary-predecessor value journal: it was removed
# because those values are unreachable after a correct reverse head rollback, and a mutation
# targeting state that no longer exists would be dead weight that always "passes" -- the exact
# anti-pattern the rest of this file is built to avoid.
#
# Nor is there one for `predecessors_.resize(journal_.predecessor_count)`.  I wrote that
# mutation and it was NOT caught, so I checked why rather than leaving it in as an always-green
# entry.  It is the same invariant one level out: a position is only ever reachable as a chain
# candidate through a head that install_anchor() published, and that same call WROTE
# predecessors_ for the position before publishing it -- so no stale predecessor value is ever
# read, whether it survived in the vector or in a journal.  The truncation stays as a
# PARALLEL-VECTOR REPRESENTATION INVARIANT at negligible cost: predecessors_ and occurrences_
# describe the same positions and are kept the same length.  It is NOT behaviourally
# load-bearing -- my earlier note that build() depends on it was wrong, since build() resizes
# to occurrences_.size() regardless -- so there is no honest gate for it.
#
# Usage:  ./p29_journal_mutations.sh
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# A fixed shared path plus `rm -rf` means two concurrent gate runs erase each other's
# evidence -- and the loser reports on files the winner replaced.  Default to a private dir.
WORK=${WORK:-$(mktemp -d /tmp/p29journalmut.XXXXXX)}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror}

mkdir -p "$WORK"
build_and_run() { # build_and_run <dir> ; echoes PASS or FAIL
  local dir=$1
  # The test includes the header with QUOTES, so the compiler searches the directory of the
  # INCLUDING FILE first -- ahead of any -I.  Compiling $HERE's test with -I"$dir" therefore
  # picks up the unmutated header and every mutation silently "passes".  The test has to be
  # copied in beside the header it is meant to be built against.
  cp "$HERE/p29_prepare_commit_test.cpp" "$dir/"
  g++ $CXXFLAGS "$dir/p29_prepare_commit_test.cpp" -o "$dir/t" 2>"$dir/build.err" \
    || { echo BUILD_ERROR; return 0; }
  if "$dir/t" >"$dir/run.out" 2>"$dir/run.err"; then echo PASS; else echo FAIL; fi
}

# Control: the unmutated header must pass, or every rejection below is meaningless.
mkdir -p "$WORK/control"; cp "$HERE/p29_online_s1.h" "$WORK/control/"
[ "$(build_and_run "$WORK/control")" = PASS ] || {
  echo "CONTROL FAILED: the unmutated header does not pass the gates" >&2
  cat "$WORK/control/build.err" "$WORK/control/run.err" 2>/dev/null >&2; exit 1; }
echo "control: unmutated header PASSES"

mutate() { # mutate <name> <sed script...>
  local name=$1; shift
  mkdir -p "$WORK/$name"; cp "$HERE/p29_online_s1.h" "$WORK/$name/"
  for s in "$@"; do sed -i "$s" "$WORK/$name/p29_online_s1.h"; done
  if cmp -s "$HERE/p29_online_s1.h" "$WORK/$name/p29_online_s1.h"; then
    # A mutation that changes nothing must never be reported as a pass -- that is exactly how
    # this script reported five clean "passes" on its first run.  A stale pattern is a hard
    # error, not a result.
    echo "MUTATION $name CHANGED NOTHING -- the sed pattern no longer matches the header" >&2
    exit 1
  fi
  RESULT[$name]=$(build_and_run "$WORK/$name")
}

declare -A RESULT
# M1: heads are never restored -- the pure "size-only rollback".
mutate no_head_restore '/heads_\[journal_\.heads\[i\]\.first\] = journal_\.heads\[i\]\.second;/d'
# M2: heads restored in FORWARD order, so a slot written twice keeps the wrong old value.
mutate forward_head_restore 's/for (size_t i = journal_\.heads\.size(); i-- > 0;)/for (size_t i = 0; i < journal_.heads.size(); ++i)/'
# M3: the occurrence stream is not truncated.
mutate no_occurrence_truncate '/occurrences_\.resize(journal_\.occurrences);/d'
# M4: the pending guard on direct admission is dropped, so a GLOBAL admit() can land inside a
# live route transaction.  The hazard is NOT un-journalled mutation -- with a transaction
# pending, install_anchor() still journals.  It is that the admitted TU FOLDS INTO the pending
# transaction, appending to the same occurrence stream and the same journal, while
# pending_plan_ still describes only the prepared TU: commit() and abort() then act on an
# ill-defined two-TU transaction.
mutate no_pending_admit_guard 's/            throw std::logic_error("S1 admit while a transaction is already pending");/            (void)0;/'
# M5b: the guard REJECTS, but only after the TU has already been built into the pending
# transaction.  It still throws, so a gate that checks "it threw" passes -- which is why
# gate 5 checks the invariant (nothing moved before the throw) instead.
mutate admit_builds_before_rejecting \
  's|^        if (pending_) {$|        if (pending_) { build(current_regions);|' \
  's|^            throw std::logic_error("S1 admit while a transaction is already pending");$|            throw std::logic_error("S1 admit while a transaction is already pending");|'

# These must be caught: they are the failures the journal exists to prevent, and each one
# changes an answer a later TU depends on.
required=(no_head_restore forward_head_restore no_occurrence_truncate no_pending_admit_guard admit_builds_before_rejecting)
bad=0
for m in "${required[@]}"; do
  if [ "${RESULT[$m]}" = FAIL ]; then
    printf 'caught   %-24s %s\n' "$m" "$(head -1 "$WORK/$m/run.err")"
  else
    printf 'NOT CAUGHT %-22s (%s)\n' "$m" "${RESULT[$m]}" >&2; bad=1
  fi
done


[ "$bad" -eq 0 ] || { echo "required mutation coverage incomplete" >&2; exit 1; }
echo "every journal failure the gates must catch is caught, and the control passes"
