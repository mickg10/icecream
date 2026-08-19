#!/usr/bin/env bash
# selector_1f_costing_mutations.sh — prove selector_1f_costing.sh can FAIL.
#
# A fail-closed launcher whose failure path is never exercised is just another claim.  This
# runs the launcher against a shim that wraps the real codec and damages exactly one thing
# per mode, and requires the launcher to reject each one — plus an UNMUTATED control that
# must be accepted, because a launcher that rejects everything proves nothing either.
#
# The mode names match the numbered checks in selector_1f_costing.sh:
#   rc       1  process exits non-zero with every output intact.  This is the exact case the
#               pre-fix launcher accepted: it published the real numbers from a run that had
#               already failed its own checks, because it keyed off `byte-exact=OK` — which
#               the codec prints BEFORE the closure/coverage/total checks run.
#   be       2  byte-exact is not OK
#   nomarker 3  a check line is missing from stdout
#   hdr      4  a TSV column is renamed under the index parse
#   row      5  one TU row is missing
#   cf       6  the C->F file is one byte longer than the TSV costs
#   win      7  a row value is edited so the TSV no longer reproduces the printed tallies
#
# Usage:  ./selector_1f_costing_mutations.sh [project/profile]
#   BIN=<codec50-sink>  MX=<ii-matrix>  WORK=<scratch>
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LAUNCH=$HERE/selector_1f_costing.sh
BIN=${BIN:-/tmp/tagreg/build/codec50-sink}
WORK=${WORK:-/tmp/selcost-mut}
CELL=${1:-fmt/debian-gcc}

[ -x "$LAUNCH" ] || { echo "not executable: $LAUNCH" >&2; exit 1; }
[ -x "$BIN" ] || { echo "codec binary not executable: $BIN" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
SHIM=$WORK/shim.sh
cat >"$SHIM" <<EOF
#!/usr/bin/env bash
REAL=$BIN
EOF
cat >>"$SHIM" <<'EOF'
if [ "$MUT" = be ] || [ "$MUT" = nomarker ]; then
  o=$(mktemp)
  "$REAL" "$@" >"$o"; rc=$?
  case $MUT in
    be)       sed -i 's/byte-exact=OK/byte-exact=FAIL/' "$o" ;;
    nomarker) sed -i '/SELECTOR full total:/d' "$o" ;;
  esac
  cat "$o"; rm -f "$o"; exit $rc
fi
"$REAL" "$@"; rc=$?
tsv=; cf=; prev=
for a in "$@"; do
  case $prev in --selector-tsv) tsv=$a;; --cf-sink) cf=$a;; esac
  prev=$a
done
case "$MUT" in
  rc)  exit 2 ;;
  hdr) sed -i '1s/global_full/global_total/' "$tsv" ;;
  row) sed -i '$d' "$tsv" ;;
  cf)  printf '\x00' >> "$cf" ;;
  win) awk -F'\t' -v OFS='\t' 'NR==2{$3=1}1' "$tsv" >"$tsv.m" || exit 9; mv "$tsv.m" "$tsv" ;;
esac
exit $rc
EOF
chmod +x "$SHIM"

# The control.  If this does not pass, every rejection below is meaningless.
if ! BIN=$BIN WORK=$WORK/control OUT=$WORK/control/results "$LAUNCH" "$CELL" >"$WORK/control.out" 2>"$WORK/control.err"; then
  echo "CONTROL FAILED: the unmutated launcher rejected $CELL" >&2
  sed 's/^/  /' "$WORK/control.err" >&2
  exit 1
fi
echo "control: unmutated $CELL accepted"

declare -A WANT=(
  [rc]='codec exited'
  [be]='byte-exact is not OK'
  [nomarker]='missing check line'
  [hdr]='unexpected selector TSV header'
  [row]='rows, expected one per TU'
  [cf]='but the C->F file holds'
  [win]='the TSV rows give'
)
bad=0
for m in rc be nomarker hdr row cf win; do
  rc=0
  MUT=$m BIN=$SHIM WORK=$WORK/$m OUT=$WORK/$m/results "$LAUNCH" "$CELL" \
      >"$WORK/$m.out" 2>"$WORK/$m.err" || rc=$?
  msg=$(head -1 "$WORK/$m.err" || true)
  if [ "$rc" -eq 0 ]; then
    echo "NOT CAUGHT  $m: the launcher accepted a mutated run" >&2; bad=1; continue
  fi
  case $msg in
    *"${WANT[$m]}"*) printf 'caught %-9s exit=%d  %s\n' "$m" "$rc" "$msg" ;;
    *) echo "WRONG CHECK $m: rejected, but not by the check this mutation targets: $msg" >&2; bad=1 ;;
  esac
done
[ "$bad" -eq 0 ] || { echo "mutation coverage incomplete" >&2; exit 1; }
echo "all 7 checks fire on the mutation each one targets, and the control still passes"
