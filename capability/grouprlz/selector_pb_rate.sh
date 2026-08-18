#!/usr/bin/env bash
# INTERIM policy-B rate binding, conservative two-pass P29 charge.
#
# P29 is two-pass today (raw-plane plan, then grouped encode). BOTH passes are inside
# the candidate clock, so every P29 C rate here is a PESSIMISTIC FLOOR: a future one-pass
# encoder can only be faster. Cells where P29 clears the gate under this charge clear it
# a fortiori; cells that fail are exactly the ones a one-pass refinement would target.
#
# Each candidate is charged its own input conversion: GRZ pays concat + TU map, P29 pays
# its own file open + intern in-process. Nothing is inferred by subtracting a stage.
#
# Asymmetry that must stay labelled: GRZ F is a real standalone single-thread decode of
# the wire; P29 has no standalone decoder, so its F is the codec's own in-process
# per-stream proxy. That is a direct codec report, not a subtraction, but it is not the
# same measurement as GRZ's.
set -euo pipefail

P=${1:?project}; PR=${2:?profile}
PROBE_REPS=${PROBE_REPS:-3}
FULL_REPS=${FULL_REPS:-2}
P29_CORES=${P29_CORES:-16-23}
GRZ_CORES=${GRZ_CORES:-24-31}
ROOT=${ROOT:-$HOME/selbind/rate}
W=$ROOT/$P-$PR
CELL=$HOME/ictmp/ii-matrix/$P/$PR
IDDIR=$HOME/selbind/pb/$P-$PR/id
P29=${P29:-$HOME/selbind/p29build/codec50-56c1744}
GRZ=${GRZ:-$HOME/issue16-selector-v1/tools/grz2g-selector}

[[ ! -e $W ]] || { echo "run dir exists: $W" >&2; exit 1; }
[[ -f $CELL/corpus.json && -f $IDDIR/prefix/literal.wire ]] || { echo "cell/gate missing: $P/$PR" >&2; exit 1; }
mkdir -p "$W"/{ex,p29,grz}

num() { [[ $1 =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "unparsable $2: '$1'" >&2; exit 3; }; printf '%s' "$1"; }
wall() { awk '{print $1}' "$1"; }

read -r PAYLOAD RAW_DECL TU_DECL < <(python3 -c "
import json,sys; d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['raw_bytes'], d['tu_count'])" "$CELL/corpus.json")

zstd -d --long=31 -q -c "$CELL/$PAYLOAD" | tar -xf - -C "$W/ex"
awk -F'\t' -v d="$W/ex/" 'NR>1{print d $2}' "$CELL/manifest.tsv" > "$W/manifest.txt"
TUS=$(wc -l < "$W/manifest.txt")
[[ $TUS -eq $TU_DECL ]] || { echo "TU mismatch $TUS/$TU_DECL" >&2; exit 5; }
PROBE=$(( TUS < 112 ? TUS : 112 ))
head -n "$PROBE" "$W/manifest.txt" > "$W/manifest.probe.txt"

P29C=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 4 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
      --blob-zstd-workers 2 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
BLIND=(); (( PROBE < TUS )) && BLIND=(--open-final-entropy)
GRZP=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8)
TF='%e %U %S %M'

# ================= bounded TU112 probe race, fixed 16-core budget, disjoint 8/8 =======
: > "$W/probe.reps"
for rep in $(seq 1 "$PROBE_REPS"); do
    R=$W/probe$rep; mkdir -p "$R"/{p29,grz}
    tr '\n' '\0' < "$W/manifest.probe.txt" | xargs -0 cat > "$R/probe.ii"
    "$GRZ" tu "$W/manifest.probe.txt" "$R/probe.tu" > /dev/null
    cat > "$R/p29.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
taskset -c $P29_CORES $P29 --manifest $W/manifest.probe.txt ${P29C[*]} --mixed-dump-prefix $R/p29/plan > $R/p29/plan.out 2> $R/p29/plan.err
taskset -c $P29_CORES $P29 --manifest $W/manifest.probe.txt ${P29C[*]} --literal-group-prefix $R/p29/plan --literal-group-tus 112 --stable-root-tags ${BLIND[*]:-} --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire $R/p29/literal.wire > $R/p29/g.out 2> $R/p29/g.err
SH
    cat > "$R/grz.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
taskset -c $GRZ_CORES $GRZ enc $R/probe.ii $R/grz/probe.grz -u $R/probe.tu ${GRZP[*]} > $R/grz/e.out 2> $R/grz/e.err
SH
    chmod +x "$R/p29.sh" "$R/grz.sh"
    T0=$(date +%s.%N)
    /usr/bin/time -f "$TF" -o "$R/p29.res" "$R/p29.sh" & A=$!
    /usr/bin/time -f "$TF" -o "$R/grz.res" "$R/grz.sh" & B=$!
    SA=0; SB=0; wait $A || SA=$?; wait $B || SB=$?
    T1=$(date +%s.%N)
    (( SA == 0 && SB == 0 )) || { echo "probe rep$rep failed p29=$SA grz=$SB" >&2; exit 6; }
    cmp -s "$R/p29/literal.wire" "$IDDIR/prefix/literal.wire" || { echo "probe rep$rep P29 wire != gated identity wire" >&2; exit 7; }
    printf '%s\t%s\t%s\t%s\n' "$rep" "$(echo "$T1-$T0"|bc)" "$(cat "$R/p29.res")" "$(cat "$R/grz.res")" >> "$W/probe.reps"
    rm -rf "$R/probe.ii" "$R"/p29/plan.*.raw
done

# ================= complete-program C, each candidate charged its own input ===========
: > "$W/p29.full.reps"
for rep in $(seq 1 "$FULL_REPS"); do
    /usr/bin/time -f "$TF" -o "$W/p29/plan.$rep.res" taskset -c "$P29_CORES" "$P29" \
        --manifest "$W/manifest.txt" "${P29C[@]}" --mixed-dump-prefix "$W/p29/plan" \
        > "$W/p29/plan.$rep.out" 2> "$W/p29/plan.$rep.err"
    /usr/bin/time -f "$TF" -o "$W/p29/g.$rep.res" taskset -c "$P29_CORES" "$P29" \
        --manifest "$W/manifest.txt" "${P29C[@]}" --literal-group-prefix "$W/p29/plan" \
        --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 \
        --literal-group-skip-zstd10 --literal-group-wire "$W/p29/literal.wire" \
        > "$W/p29/g.$rep.out" 2> "$W/p29/g.$rep.err"
    printf '%s\t%s\t%s\t%s\t%s\n' "$rep" "$(wall "$W/p29/plan.$rep.res")" "$(wall "$W/p29/g.$rep.res")" \
        "$(cat "$W/p29/plan.$rep.res")" "$(cat "$W/p29/g.$rep.res")" >> "$W/p29.full.reps"
    rm -f "$W"/p29/plan.*.raw
done
P29_WIRE=$(num "$(grep -o 'TOTAL=[0-9]*' "$W/p29/g.1.out" | head -1 | cut -d= -f2)" p29_wire)
P29_EXACT=$(grep -o 'byte-exact=[A-Z]*' "$W/p29/g.1.out" | head -1 | cut -d= -f2)
P29_F=$(sed -n 's/.*F-decode \([0-9.]*\) GB\/s.*/\1/p' "$W/p29/g.1.err" | tail -1)

: > "$W/grz.full.reps"
for rep in $(seq 1 "$FULL_REPS"); do
    /usr/bin/time -f "$TF" -o "$W/grz/cat.$rep.res" bash -c "tr '\n' '\0' < $W/manifest.txt | xargs -0 cat > $W/grz/cell.ii"
    /usr/bin/time -f "$TF" -o "$W/grz/tu.$rep.res" "$GRZ" tu "$W/manifest.txt" "$W/grz/cell.tu" > /dev/null
    /usr/bin/time -f "$TF" -o "$W/grz/enc.$rep.res" taskset -c "$GRZ_CORES" "$GRZ" enc \
        "$W/grz/cell.ii" "$W/grz/cell.grz" -u "$W/grz/cell.tu" "${GRZP[@]}" \
        > "$W/grz/enc.$rep.out" 2> "$W/grz/enc.$rep.err"
    printf '%s\t%s\t%s\t%s\t%s\n' "$rep" "$(wall "$W/grz/cat.$rep.res")" "$(wall "$W/grz/tu.$rep.res")" \
        "$(wall "$W/grz/enc.$rep.res")" "$(cat "$W/grz/enc.$rep.res")" >> "$W/grz.full.reps"
done
GRZ_WIRE=$(num "$(cut -f2 "$W/grz/enc.1.out")" grz_wire)

# ---- GRZ F: real standalone single-thread decode, plus one byte-exact round trip -----
: > "$W/grz.dec.reps"
for rep in $(seq 1 "$FULL_REPS"); do
    /usr/bin/time -f "$TF" -o "$W/grz/dec.$rep.res" taskset -c "$GRZ_CORES" "$GRZ" dec \
        "$W/grz/cell.grz" /dev/null -j 1 > /dev/null 2> "$W/grz/dec.$rep.err"
    printf '%s\t%s\t%s\n' "$rep" "$(wall "$W/grz/dec.$rep.res")" \
        "$(sed -n 's/.* F=\([0-9]*\) B\/s.*/\1/p' "$W/grz/dec.$rep.err")" >> "$W/grz.dec.reps"
done
taskset -c "$GRZ_CORES" "$GRZ" dec "$W/grz/cell.grz" "$W/grz/replay.ii" -j 1 > /dev/null 2>&1
if cmp -s "$W/grz/cell.ii" "$W/grz/replay.ii"; then GRZ_EXACT=YES; else GRZ_EXACT=NO; fi
rm -f "$W/grz/replay.ii" "$W/grz/cell.ii"

# ================= row ===============================================================
python3 - "$W" "$P" "$PR" "$TUS" "$PROBE" "$RAW_DECL" "$P29_WIRE" "$GRZ_WIRE" \
    "$P29_EXACT" "$GRZ_EXACT" "${P29_F:-NA}" > "$W/row.tsv" <<'PY'
import sys, statistics
W,P,PR,TUS,PROBE,RAW,P29W,GRZW,P29X,GRZX,P29F = sys.argv[1:12]
RAW=int(RAW)
def rows(f):
    return [l.rstrip("\n").split("\t") for l in open(f"{W}/{f}") if l.strip()]
p29=[(float(r[1]),float(r[2])) for r in rows("p29.full.reps")]          # plan, grouped
grz=[(float(r[1]),float(r[2]),float(r[3])) for r in rows("grz.full.reps")]  # cat, tu, enc
dec=[(float(r[1]),int(r[2])) for r in rows("grz.dec.reps")]
pr =[(float(r[1]),) for r in rows("probe.reps")]
p29_c=[RAW/1e9/(a+b) for a,b in p29]
grz_c=[RAW/1e9/(a+b+c) for a,b,c in grz]
grz_f=[d[1]/1e9 for d in dec]
out={"project":P,"profile":PR,"total_tus":TUS,"probe_tus":PROBE,"raw_bytes":RAW,
     "p29_complete_bytes":P29W,"grz_complete_bytes":GRZW,
     "p29_complete_exact":P29X,"grz_complete_exact":GRZX,
     "p29_C_gbps_2pass_med":"%.4f"%statistics.median(p29_c),
     "p29_C_gbps_2pass_all":",".join("%.4f"%x for x in p29_c),
     "p29_plan_s_all":",".join("%.2f"%a for a,_ in p29),
     "p29_grouped_s_all":",".join("%.2f"%b for _,b in p29),
     "grz_C_gbps_med":"%.4f"%statistics.median(grz_c),
     "grz_C_gbps_all":",".join("%.4f"%x for x in grz_c),
     "grz_cat_s_all":",".join("%.2f"%a for a,_,_ in grz),
     "grz_enc_s_all":",".join("%.2f"%c for _,_,c in grz),
     "grz_F_gbps_med":"%.4f"%statistics.median(grz_f),
     "grz_F_gbps_all":",".join("%.4f"%x for x in grz_f),
     "p29_F_gbps_codec_proxy":P29F,
     "probe_makespan_all":",".join("%.3f"%r[0] for r in pr),
     "probe_makespan_med":"%.3f"%statistics.median(r[0] for r in pr),
     "p29_C_legal_2pass":str(statistics.median(p29_c)>=1.0),
     "grz_C_legal":str(statistics.median(grz_c)>=1.0),
     "grz_F_legal":str(statistics.median(grz_f)>=0.5),
     "charge":"INTERIM conservative two-pass P29 (plan+grouped in clock); GRZ charged concat+TU map",
     "p29_F_basis":"codec in-process per-stream proxy (no standalone P29 decoder)",
     "grz_F_basis":"standalone single-thread decode of the wire"}
for k,v in out.items(): print(f"{k}\t{v}")
PY

find "$W/ex" -mindepth 1 -delete; rmdir "$W/ex"
echo "RATE_DONE $P/$PR"
