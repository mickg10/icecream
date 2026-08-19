#!/usr/bin/env bash
# selector_1f_costing.sh — per-TU RAW vs GLOBAL_S1 costing at 1F (--route-s1 1).
#
# WHAT THIS MEASURES, and what it does not.  Every row is a DIFFERENTIAL ON THE ALWAYS-GLOBAL
# BASELINE STATE: the codec runs GLOBAL_S1 throughout and prices what RAW would have cost for
# that TU against the same history.  It is NOT the live selector curve.  The moment RAW wins a
# TU for real, the F mirror acquires Block holes, and every later candidate cost has to be
# recomputed against that SELECTED history rather than against the global one.  So the RAW-win
# percentages here bound how often RAW would be preferred at the first divergence, not how a
# live selector would behave over a whole build.  The live curve comes from the
# prepare/choose/Ack integration, which emits its own selected-history TSV.
#
# This launcher is FAIL CLOSED.  The previous version began with `set +e`, ignored the
# codec's exit status, and accepted a cell on the log containing `byte-exact=OK` — which
# the codec prints BEFORE the closure/manifest/full-total checks run.  So a mutation could
# fire one of those checks, exit non-zero, and still be counted as a successful cell: the
# acceptance criterion did not depend on the result of the checks it was supposed to
# enforce.  That is the same "gate that cannot fail" pattern the checks themselves were
# added to remove, one level up in the harness.
#
# A cell is accepted only if ALL of these hold:
#   1. the codec PROCESS exits 0                          (status, not a log marker)
#   2. stdout carries byte-exact=OK                       (byte-exactness has no exit code)
#   3. all three SELECTOR check lines are present         (closure / manifest / full total)
#   4. the TSV header is exactly the expected columns     (we parse by column index)
#   5. the TSV has exactly one row per TU
#   6. sum(global_full) over the TSV == the SIZE OF THE C->F SINK FILE on disk
#   7. the TSV's own tallies reproduce the codec's printed tallies
#
# Check 6 is the one that does not go through the codec: both sides are read back from the
# filesystem after the process has exited, so no in-process counter can make it agree with
# itself.  Checks 1-5 and 7 are conjuncts, never substitutes for each other.
#
# A failing cell keeps its working directory and aborts the whole run non-zero.  Nothing is
# summarised from a cell that did not pass — a partially-checked number is precisely what
# this harness exists to prevent.
#
# Usage:  ./selector_1f_costing.sh [project/profile ...]
#   MX=<ii-matrix>  BIN=<codec50-sink>  WORK=<scratch>  OUT=<results dir>  REPS=<builds>
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/selector_evidence.sh"

MX=${MX:-$HOME/ictmp/ii-matrix}
BIN=${BIN:-/tmp/tagreg/build/codec50-sink}
WORK=${WORK:-/tmp/selcost}
OUT=${OUT:-$WORK/results}
REPS=${REPS:-4}

CELL=""
T=""
fail() {
  echo "CELL FAIL [${CELL:-<setup>}]: $*" >&2
  if [ -n "$T" ] && [ -d "$T" ]; then
    echo "  working directory retained: $T (out, err, sel.tsv, s.cf, s.fc)" >&2
  fi
  exit 1
}
trap 'rc=$?; [ $rc -eq 0 ] || fail "aborted with status $rc at line $LINENO"' ERR

[ -x "$BIN" ] || fail "codec binary not executable: $BIN (build it with selector_build_codec50_sink.sh)"
[ -d "$MX" ] || fail "ii-matrix not found: $MX"

# The exact binding used for every 1F costing number on this branch.
BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --route-s1 1)

# Parsed by column index below, so a rename or reorder must be a hard error, not a silent
# mis-read of some other column.
WANT_HDR=$'tu\traw_root_bytes\traw_root_z\tglobal_root_bytes\tglobal_root_z\tglobal_new_blocks\tglobal_blockdef_bytes\tglobal_blockdef_z\tglobal_diff_z\tcommon_physical\traw_full\tglobal_full\twinner'

CELLS=("$@")
[ ${#CELLS[@]} -gt 0 ] || CELLS=(re2/debian-gcc fmt/debian-gcc cereal/debian-gcc leveldb/debian-gcc spdlog/debian-gcc)

mkdir -p "$OUT"
selector_evidence_init "$OUT" "$BIN"
RESULTS=$OUT/selector-1f-per-tu-costing.tsv
printf '# differential on the always-GLOBAL baseline; not the live selector curve\n' >"$RESULTS"
printf 'cell\tTUs\traw_cum\tglobal_cum\tRAW_wins\tGLOBAL_wins\tties\tRAW_win_pct\traw_full_total\tglobal_full_total\tcf_bytes\n' >>"$RESULTS"

for cell in "${CELLS[@]}"; do
  CELL=$cell
  P=${cell%%/*}; PR=${cell##*/}
  T=$WORK/$P.$PR; rm -rf "$T"; mkdir -p "$T/ii"

  J=$MX/$P/$PR/corpus.json
  [ -f "$J" ] || fail "no corpus.json at $J"
  # corpus.json names the authoritative payload.  Never glob the cell directory: cells can
  # hold a superseded second tarball, and a glob silently picks the wrong one.
  meta=$(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])" "$J")
  read -r REL SHA N <<<"$meta"
  have=$(sha256sum "$MX/$P/$PR/$REL" | cut -d' ' -f1)
  [ "$have" = "$SHA" ] || fail "payload sha256 mismatch: $have != $SHA ($REL)"

  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C "$T/ii"
  find "$T/ii" -name '*.ii' | sort >"$T/man"
  M=$(wc -l <"$T/man")
  [ "$M" = "$N" ] || fail "extracted $M .ii files but corpus.json declares tu_count=$N"
  : >"$T/man$REPS"
  for ((i=0; i<REPS; ++i)); do cat "$T/man" >>"$T/man$REPS"; done
  TU=$((N * REPS))

  # 1. the process status, captured and required — separately from anything it printed.
  #    stdout and stderr go to separate files: the combined stream interleaves the codec's
  #    buffered summary lines with the unbuffered stderr RSS line, so greps on a merged log
  #    are not reliable.
  rc=0
  "$BIN" --manifest "$T/man$REPS" "${BASE[@]}" \
         --selector-tsv "$T/sel.tsv" --cf-sink "$T/s.cf" --fc-sink "$T/s.fc" \
         >"$T/out" 2>"$T/err" || rc=$?
  [ "$rc" -eq 0 ] || fail "codec exited $rc"

  # 2-3. required stdout evidence.  Necessary, never sufficient: they are conjuncts of the
  #      exit status above, because byte-exactness alone has no exit code of its own.
  grep -qF 'byte-exact=OK' "$T/out" || fail "byte-exact is not OK"
  for marker in 'SELECTOR closure:' 'SELECTOR manifest:' 'SELECTOR full total:'; do
    grep -qF "$marker" "$T/out" || fail "missing check line: $marker"
  done

  # 4-5. the TSV's own shape.
  [ -s "$T/sel.tsv" ] || fail "no selector TSV was written"
  hdr=$(head -1 "$T/sel.tsv")
  [ "$hdr" = "$WANT_HDR" ] || fail "unexpected selector TSV header (columns are parsed by index)"
  stats=$(awk -F'\t' 'NR>1 {
        r++; a += $3; g += $9; rf += $11; gf += $12;
        if ($9 > $3) rw++; else if ($9 < $3) gw++; else tw++
      }
      END { printf "%d %.0f %.0f %.0f %.0f %d %d %d", r, a, g, rf, gf, rw+0, gw+0, tw+0 }' "$T/sel.tsv")
  read -r rows raw_cum global_cum raw_full global_full raw_wins global_wins ties <<<"$stats"
  [ "$rows" = "$TU" ] || fail "selector TSV has $rows rows, expected one per TU ($TU = $N x $REPS)"

  # 6. the independent one: the TSV read back from disk must sum to the SIZE of the C->F
  #    file on disk.  Neither side is an in-process counter, so the codec cannot make this
  #    agree with itself.
  [ -f "$T/s.cf" ] || fail "no C->F sink file was written"
  cf_size=$(stat -c %s "$T/s.cf")
  [ "$global_full" = "$cf_size" ] || fail "TSV global_full sums to $global_full but the C->F file holds $cf_size bytes ($((global_full - cf_size)) off)"

  # 7. the codec's printed tallies must reproduce from the rows it wrote.
  costing=$(grep -F 'SELECTOR per-TU costing:' "$T/out") || fail "no per-TU costing line"
  totals=$(grep -F 'SELECTOR FULL transaction totals:' "$T/out") || fail "no full-transaction totals line"
  p_rows=$(sed 's/.*rows=\([0-9]*\).*/\1/' <<<"$costing")
  p_raw=$(sed 's/.*raw_cum=\([0-9]*\).*/\1/' <<<"$costing")
  p_glob=$(sed 's/.*global_cum=\([0-9]*\).*/\1/' <<<"$costing")
  p_rw=$(sed 's/.*RAW=\([0-9]*\).*/\1/' <<<"$costing")
  p_gw=$(sed 's/.*GLOBAL_S1=\([0-9]*\).*/\1/' <<<"$costing")
  p_tie=$(sed 's/.*tie=\([0-9]*\)\].*/\1/' <<<"$costing")
  p_rawfull=$(sed 's/.*raw=\([0-9]*\).*/\1/' <<<"$totals")
  p_globfull=$(sed 's/.* global=\([0-9]*\).*/\1/' <<<"$totals")
  for pair in "rows:$p_rows:$rows" "raw_cum:$p_raw:$raw_cum" "global_cum:$p_glob:$global_cum" \
              "RAW_wins:$p_rw:$raw_wins" "GLOBAL_wins:$p_gw:$global_wins" "ties:$p_tie:$ties" \
              "raw_full:$p_rawfull:$raw_full" "global_full:$p_globfull:$global_full"; do
    IFS=: read -r what printed derived <<<"$pair"
    [ "$printed" = "$derived" ] || fail "$what: the codec printed $printed, the TSV rows give $derived"
  done

  cp "$T/sel.tsv" "$OUT/sel.$P.$PR.tsv"
  cp "$T/out" "$OUT/log.$P.$PR.out"
  # Retain the ACCEPTED cell too, not just the failed ones: stdout, stderr, both physical
  # streams, sizes + SHA-256, and the command line that produced them.
  selector_evidence_cell "$OUT" "$P.$PR" "$BIN --manifest <man x$REPS> ${BASE[*]} --selector-tsv sel.tsv --cf-sink s.cf --fc-sink s.fc" \
      "$T/out" "$T/err" "$T/sel.tsv" "$T/s.cf" "$T/s.fc"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.2f%%\t%s\t%s\t%s\n' \
      "$P" "$TU" "$raw_cum" "$global_cum" "$raw_wins" "$global_wins" "$ties" \
      "$(awk -v a="$raw_wins" -v b="$TU" 'BEGIN{printf "%.4f", a*100/b}')" \
      "$raw_full" "$global_full" "$cf_size" >>"$RESULTS"
  echo "OK $P/$PR: $TU TUs, C->F $cf_size bytes, RAW wins $raw_wins, GLOBAL_S1 wins $global_wins, ties $ties"
  rm -rf "$T"; T=""
done

CELL=""
echo
cat "$RESULTS"
echo
echo "all ${#CELLS[@]} cell(s) passed every check; results in $RESULTS"
