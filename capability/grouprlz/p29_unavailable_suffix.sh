#!/usr/bin/env bash
# Strict staged-input gate for the Phase-B shared-C/F reference state.
#
# The driver starts without a manifest.  In each of two independent sessions TU2's path does
# not exist at READY, while TU1 is submitted, or when TU1's Ack/event row is received.  Only
# then is TU2 created and its path submitted.  The two sessions use different TU2 bytes and
# must produce byte-identical TU1 Ack/event rows.
#
# This proves the incremental state/API boundary.  selector_prefix_invariance.sh separately
# checks the current codec's physical frame prefix, whose loader is still honestly preloaded.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$(mktemp -d /tmp/p29-unavailable-suffix.XXXXXX)}
CXXFLAGS=${CXXFLAGS:-${ICE_CXX_STANDARD_FLAG:--std=c++23} -O2 -pthread -Wall -Wextra -Wpedantic -Werror}

fail() {
  echo "UNAVAILABLE-SUFFIX FAIL: $*" >&2
  echo "  evidence retained: $WORK" >&2
  exit 1
}
on_error() {
  local status=$?
  echo "UNAVAILABLE-SUFFIX FAIL: aborted with status $status" >&2
  echo "  evidence retained: $WORK" >&2
  exit "$status"
}
trap on_error ERR

mkdir -p "$WORK"
read -r -a CXX_ARGS <<<"$CXXFLAGS"
g++ "${CXX_ARGS[@]}" "$HERE/p29_unavailable_suffix_driver.cpp" -o "$WORK/driver"

PREFIX=$WORK/tu1.ii
printf '%s\n' \
  'template<class T> T twice(T value) { return value + value; }' \
  'int current_tu() { return twice(21); }' >"$PREFIX"

run_case() { # run_case <tag> <suffix line 1> <suffix line 2>
  local tag=$1 line1=$2 line2=$3 dir=$WORK/$1 suffix=$WORK/$1/tu2.ii
  local input_fifo=$dir/input.fifo output_fifo=$dir/output.fifo pid to_driver from_driver
  local ready ack1 ack2 done_line
  mkdir -p "$dir"
  [ ! -e "$suffix" ] || fail "$tag: TU2 exists before the C/F process starts"
  mkfifo "$input_fifo" "$output_fifo"
  timeout 60 "$WORK/driver" <"$input_fifo" >"$output_fifo" 2>"$dir/driver.err" &
  pid=$!
  exec {to_driver}>"$input_fifo"
  exec {from_driver}<"$output_fifo"

  IFS= read -r ready <&"$from_driver"
  [ "$ready" = READY ] || fail "$tag: driver did not reach READY"
  [ ! -e "$suffix" ] || fail "$tag: TU2 exists at READY"
  printf 'TU\t%s\n' "$PREFIX" >&"$to_driver"
  IFS= read -r ack1 <&"$from_driver"
  [[ $ack1 == $'ACK\t0\t'* ]] || fail "$tag: TU1 did not produce its Ack/event row"
  [ ! -e "$suffix" ] || fail "$tag: TU2 existed before TU1 Ack"

  printf '%s\n%s\n' "$line1" "$line2" >"$suffix"
  printf 'TU\t%s\n' "$suffix" >&"$to_driver"
  IFS= read -r ack2 <&"$from_driver"
  [[ $ack2 == $'ACK\t1\t'* ]] || fail "$tag: TU2 did not produce its Ack/event row"
  printf 'END\n' >&"$to_driver"
  IFS= read -r done_line <&"$from_driver"
  [[ $done_line == $'DONE\t2\t2\t2\t'* ]] || fail "$tag: final state did not close two TUs"
  exec {to_driver}>&-
  exec {from_driver}<&-
  wait "$pid" || fail "$tag: driver exited nonzero"

  printf '%s\n' "$ack1" >"$dir/tu1.ack-event.tsv"
  printf '%s\n' "$ack2" >"$dir/tu2.ack-event.tsv"
  printf '%s\n' "$done_line" >"$dir/final-state.tsv"
  CASE_ACK1=$ack1
  CASE_ACK2=$ack2
}

run_case suffix_a \
  'int future_a() { return twice(22); }' \
  'const char *future_name_a = "alpha";'
ack_a=$CASE_ACK1 second_a=$CASE_ACK2
run_case suffix_b \
  'int future_b() { return twice(999); }' \
  'const char *future_name_b = "a completely different suffix";'
ack_b=$CASE_ACK1 second_b=$CASE_ACK2

[ "$ack_a" = "$ack_b" ] || fail "TU1 Ack/event state depends on the later TU bytes"
[ "$second_a" != "$second_b" ] || fail "different staged TU2 inputs produced identical event rows"

echo "strict unavailable suffix: PASS"
echo "  TU1 path was the only submitted TU through its Ack in both sessions"
echo "  TU1 Ack/event rows are byte-identical across two different later TUs"
echo "  evidence: $WORK"
