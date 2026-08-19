#!/usr/bin/env bash
# p29_journal_mutations.sh — prove the prepare/commit/abort gates can FAIL, by breaking the
# journal in p29_online_s1.h itself and requiring the test to notice.
#
# The mutations are applied to a COPY of the production header and the unmodified test is
# compiled against it, so what is being gated is the real undo() and the real install_anchor()
# -- not a paraphrase of them living in the test.
#
# Usage:  ./p29_journal_mutations.sh
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-/tmp/p29journalmut}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror}

rm -rf "$WORK"; mkdir -p "$WORK"
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
  cmp -s "$HERE/p29_online_s1.h" "$WORK/$name/p29_online_s1.h" && {
    echo "MUTATION $name CHANGED NOTHING -- the sed pattern no longer matches the header" >&2
    return 1; }
  RESULT[$name]=$(build_and_run "$WORK/$name")
}

declare -A RESULT
# M1: heads are never restored -- the pure "size-only rollback".
mutate no_head_restore '/heads_\[journal_\.heads\[i\]\.first\] = journal_\.heads\[i\]\.second;/d'
# M2: heads restored in FORWARD order, so a slot written twice keeps the wrong old value.
mutate forward_head_restore 's/for (size_t i = journal_\.heads\.size(); i-- > 0;)/for (size_t i = 0; i < journal_.heads.size(); ++i)/'
# M3: boundary anchors into the previous TU's tail are not recorded.
mutate no_boundary_record 's/if (pending_ \&\& position < journal_\.begin) {/if (false) {/'
# M4: recorded boundary predecessors are never restored.
mutate no_predecessor_restore \
  '/for (size_t i = journal_\.predecessors\.size(); i-- > 0;)/d' \
  '/predecessors_\[journal_\.predecessors\[i\]\.first\] = journal_\.predecessors\[i\]\.second;/d'
# M5: the occurrence stream is not truncated at all.
mutate no_occurrence_truncate '/occurrences_\.resize(journal_\.occurrences);/d'

# These must be caught: they are the failures the journal exists to prevent, and each one
# changes an answer a later TU depends on.
required=(no_head_restore forward_head_restore no_occurrence_truncate)
bad=0
for m in "${required[@]}"; do
  if [ "${RESULT[$m]}" = FAIL ]; then
    printf 'caught   %-24s %s\n' "$m" "$(head -1 "$WORK/$m/run.err")"
  else
    printf 'NOT CAUGHT %-22s (%s)\n' "$m" "${RESULT[$m]}" >&2; bad=1
  fi
done

# These two are REPORTED, not required.  A boundary predecessor at q < begin is only ever
# reached through heads_[slot(q)] or through a predecessors_ link created while that head
# equalled q; heads_ is restored, and any such link belongs to the aborted TU and dies with
# the truncate.  Every later TU that can match at all has already reinstalled the boundary
# anchors the aborted TU touched, before it matches.  So the recording looks defensive rather
# than load-bearing.  It is kept because that argument depends on the current call pattern,
# and it is stated here rather than dressed up as a gate that passes.
for m in no_boundary_record no_predecessor_restore; do
  if [ "${RESULT[$m]}" = FAIL ]; then
    printf 'caught   %-24s %s\n' "$m" "$(head -1 "$WORK/$m/run.err")"
  else
    printf 'not observable  %-17s (%s) -- see the note in this script\n' "$m" "${RESULT[$m]}"
  fi
done

[ "$bad" -eq 0 ] || { echo "required mutation coverage incomplete" >&2; exit 1; }
echo "every journal failure the gates must catch is caught, and the control passes"
