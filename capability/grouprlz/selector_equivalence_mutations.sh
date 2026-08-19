#!/usr/bin/env bash
# selector_equivalence_mutations.sh — prove the two byte-equivalence gates can FAIL.
#
# selector_step1_equivalence.sh and selector_step2_equivalence.sh produced the "10/10 cells
# byte-identical" verdicts the whole T_current refactor rests on, and both were fail-open.
# Step 2's was the worse of the two: byte-exactness was checked with `... || echo "NOT
# byte-exact"`, which printed a note into the same stream as the table and did not feed the
# identity verdict at all -- two identically-broken builds would still print stable_ident=YES.
#
# Each mode below damages exactly one thing and requires the gate to reject it by the check
# that mode targets, plus an unmutated control that must still pass.
#
# Usage:  S1REF=<reference codec50-sink> ./selector_equivalence_mutations.sh [project/profile]
#   BIN=<codec50-sink under test>  MX=<ii-matrix>  WORK=<scratch>
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-$HERE/build/codec50-sink}
S1REF=${S1REF:?set S1REF to the reference codec50-sink build}
# A fixed shared path plus `rm -rf` means two concurrent gate runs erase each other's
# evidence -- and the loser reports on files the winner replaced.  Default to a private dir.
WORK=${WORK:-$(mktemp -d /tmp/eqmut.XXXXXX)}
CELL=${1:-fmt/debian-gcc}

[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 1; }
[ -x "$S1REF" ] || { echo "not executable: $S1REF" >&2; exit 1; }

mkdir -p "$WORK"
SHIM=$WORK/shim.sh
cat >"$SHIM" <<EOF
#!/usr/bin/env bash
REAL=$BIN
EOF
cat >>"$SHIM" <<'EOF'
# Only the run that carries --literal-ondemand AND a --cf-sink is damaged, so the mutation
# lands on the stream the gate compares rather than on a setup run.
tsv=; cf=; ondemand=0; prev=
for a in "$@"; do
  case $prev in --cf-sink) cf=$a;; esac
  [ "$a" = --literal-ondemand ] && ondemand=1
  prev=$a
done
target=0
[ "$ondemand" = 1 ] && [ -n "$cf" ] && target=1
if [ "$MUT" = be ] && [ "$target" = 1 ]; then
  o=$(mktemp)
  "$REAL" "$@" >"$o"; rc=$?
  sed -i 's/byte-exact=OK/byte-exact=FAIL/' "$o"
  cat "$o"; rm -f "$o"; exit $rc
fi
if [ "$MUT" = total ] && [ "$ondemand" = 1 ] && [ -z "$cf" ]; then
  o=$(mktemp)
  "$REAL" "$@" >"$o"; rc=$?
  sed -i 's/TOTAL=\([0-9]*\)/TOTAL=1\1/' "$o"
  cat "$o"; rm -f "$o"; exit $rc
fi
"$REAL" "$@"; rc=$?
[ "$target" = 1 ] || exit $rc
case "$MUT" in
  rc)    exit 2 ;;
  cf)    printf '\x00' >>"$cf" ;;
  empty) : >"$cf" ;;
esac
exit $rc
EOF
chmod +x "$SHIM"

check() { # check <gate-name> <mode> <expected substring> -- <env assignments as argv>
  local gate=$1 mode=$2 want=$3; shift 4
  local rc=0
  env "$@" MUT="$mode" WORK="$WORK/$gate.$mode" "$HERE/selector_$gate.sh" "$CELL" \
      >"$WORK/$gate.$mode.out" 2>"$WORK/$gate.$mode.err" || rc=$?
  local msg; msg=$(head -1 "$WORK/$gate.$mode.err" || true)
  if [ "$rc" -eq 0 ]; then
    echo "NOT CAUGHT  $gate/$mode: the gate accepted a mutated run" >&2; return 1
  fi
  case $msg in
    *"$want"*) printf 'caught %-28s exit=%d  %s\n' "$gate/$mode" "$rc" "$msg" ;;
    *) echo "WRONG CHECK $gate/$mode: rejected, but not by the targeted check: $msg" >&2; return 1 ;;
  esac
}

bad=0
# Controls first: rejections mean nothing if the gate rejects everything.
env BIN="$BIN" WORK="$WORK/c1" "$HERE/selector_step1_equivalence.sh" "$CELL" \
    >"$WORK/c1.out" 2>"$WORK/c1.err" || { echo "CONTROL FAILED: step1 rejected $CELL" >&2; cat "$WORK/c1.err" >&2; exit 1; }
env S1="$S1REF" S2="$BIN" WORK="$WORK/c2" "$HERE/selector_step2_equivalence.sh" "$CELL" \
    >"$WORK/c2.out" 2>"$WORK/c2.err" || { echo "CONTROL FAILED: step2 rejected $CELL" >&2; cat "$WORK/c2.err" >&2; exit 1; }
echo "controls: both gates accept the unmutated $CELL"

check step1_equivalence rc    'codec exited 2'          -- BIN="$SHIM" || bad=1
check step1_equivalence be    'byte-exact is not OK'    -- BIN="$SHIM" || bad=1
check step1_equivalence cf    'C->F streams differ'     -- BIN="$SHIM" || bad=1
check step1_equivalence empty 'is missing or empty'     -- BIN="$SHIM" || bad=1

check step2_equivalence rc    'codec exited 2'          -- S1="$S1REF" S2="$SHIM" || bad=1
check step2_equivalence be    'byte-exact is not OK'    -- S1="$S1REF" S2="$SHIM" || bad=1
check step2_equivalence cf    'C->F streams differ'     -- S1="$S1REF" S2="$SHIM" || bad=1
check step2_equivalence total 'legacy accounting TOTAL differs' -- S1="$S1REF" S2="$SHIM" || bad=1

[ "$bad" -eq 0 ] || { echo "mutation coverage incomplete" >&2; exit 1; }
echo "both equivalence gates reject every mutation by the check it targets, and both controls pass"
